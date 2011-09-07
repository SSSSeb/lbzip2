/* lbunzip2.c,v 1.40 2009/12/10 23:51:09 lacos Exp */

#include <config.h>

#include <assert.h>       /* assert() */
#include <string.h>       /* memcpy() */
#include <signal.h>       /* SIGUSR2 */

#include "yambi.h"        /* YBdec_t */

#include "main.h"         /* pname */
#include "lbunzip2.h"     /* lbunzip2() */
#include "lacos_rbtree.h" /* struct lacos_rbtree_node */


/* Number of bytes in CRC. */
#define NUM_CRC ((size_t)4)

/* Number of bytes in stream header, without block size. */
#define NUM_SHDR ((size_t)3)

/* Number of bytes in both block header and end of stream marker. */
#define NUM_BHDR ((size_t)6)

/* 48 bit mask for bzip2 block header and end of stream marker. */
static const uint64_t
    magic_mask = (uint64_t)0xFFFFlu << 32 | (uint64_t)0xFFFFFFFFlu;

/* 48 bit bzip2 block header. */
static const uint64_t
    magic_hdr = (uint64_t)0x3141lu << 32 | (uint64_t)0x59265359lu;

/* 48 bit bzip2 end of stream marker. */
static const uint64_t
    magic_eos = (uint64_t)0x1772lu << 32 | (uint64_t)0x45385090lu;

/* Bzip2 stream header, block size 9, and block header together. */
static const char unsigned intro[NUM_SHDR + (size_t)1 + NUM_BHDR] = {
    0x42u, 0x5Au, 0x68u,
    0x39u,
    0x31u, 0x41u, 0x59u, 0x26u, 0x53u, 0x59u
};

/* Bzip2 end of stream marker. */
static const char unsigned eos[NUM_BHDR] = {
    0x17u, 0x72u, 0x45u, 0x38u, 0x50u, 0x90u
};

/*
  We assume that there exists an upper bound on the size of any bzip2 block,
  i.e. we don't try to support arbitarly large blocks.
*/
#define MX_SPLIT ((size_t)(1024u * 1024u))
#define MX_BZIP2 ((size_t)(MX_SPLIT - (sizeof eos + 1u)))

struct s2w_blk                   /* Splitter to workers. */
{
  uint64_t id;                   /* Block serial number as read from stdin. */
  struct s2w_blk *next;          /* First part of next block belongs to us. */
  unsigned refno;                /* Threads not yet done with this block. */
  size_t loaded;                 /* Number of bytes in compr. */
  char unsigned compr[MX_SPLIT]; /* Data read from stdin. */
};


struct w2w_blk_id
{
  uint64_t s2w_blk_id, /* Stdin block index. */
      bzip2_blk_id;    /* Bzip2 block index within stdin block. */
  int last_bzip2;      /* Last bzip2 for stdin block. */
};


struct w2w_blk              /* Worker to workers. */
{
  struct w2w_blk_id id;     /* Stdin blk idx & bzip2 blk idx in former. */
  struct w2w_blk *next;     /* Next block in list (unordered). */
  size_t reconstructed;     /* Number of bytes in streamdata. */
  char unsigned
      streamdata[MX_BZIP2]; /* One-block bzip2 stream to decompress. */
  unsigned rbits_left;      /* After the byte @ strd[rctr-1], [1..CHAR_BIT]. */
};


struct sw2w_q                /* Splitter and workers to workers queue. */
{
  struct cond proceed;       /* See below. */
  struct s2w_blk *next_scan; /* Scan this stdin block for bzip2 blocks. */
  int eof;                   /* Splitter done with producing s2w_blk's. */
  struct w2w_blk *deco_head; /* Unordered list of bzip2 streams to decompr. */
  unsigned scanning;         /* # of workers currently producing w2w_blk's. */
};
/*
  The monitor "proceed" is associated with two predicates, because any worker
  can be in either of two needs to proceed (see work_get_first() and
  work_get_second()). We don't use two condition variables because
  - with two variables, all broadcast sites would have to consider both
    variables,
  - one of the predicates is stricter and implies the weaker one,
  - it's a rare occurrence that the weaker proceed predicate (!B) holds and the
    stricter one (!A) does not, so spurious wakeups are rare.

  The proceed predicate for work_get_first():
  !A: 0 != deco_head || 0 != next_scan || (eof && 0u == scanning)

  Necessary condition so there is any worker blocking in work_get_first():
  A: 0 == deco_head && 0 == next_scan && (!eof || 0u < scanning)

  The proceed predicate for work_get_second():
  !B: 0 != deco_head || 0 != next_scan || eof

  Necessary condition so there is any worker blocking in work_get_second():
  B: 0 == deco_head && 0 == next_scan && !eof

  B is stricter than A, B implies A,
  !A is stricter than !B, !A implies !B.

  Below, X denotes the value of the predicate or a protected member evaluated
  just after entering the monitor, before any changes are made to the protected
  members. X' denotes the value of the predicate or a protected member
  evaluated just before leaving the monitor, after all changes are made.

  We want to send as few as possible broadcasts. We can omit the broadcast if,
  before making any changes to the protected fields, neither of the necessary
  conditions for blocking hold: !A && !B, because then no worker can block.
  Otherwise (A || B), there may be a worker blocking. In that case, we only
  need to send the broadcast if we produced new tasks (enabled the proceed
  predicate) for those possibly blocking. (If we didn't enable the proceed
  predicate, then it makes no sense to wake them up.) So we'll send the
  broadcast iff this holds:

  (A && !A') || (B && !B')

  (
    (0 == deco_head && 0 == next_scan && (!eof || 0u < scanning))
    && (0 != deco_head' || 0 != next_scan' || (eof' && 0u == scanning'))
  )
  || (
    (0 == deco_head && 0 == next_scan && !eof)
    && (0 != deco_head' || 0 != next_scan' || eof')
  )

  A spurious wakeup happens when we send the broadcast because of B && !B' (the
  proceed predicate for work_get_second() becomes true from false), but !A'
  (the proceed predicate for work_get_first() evaluated after the changes) is
  false. This is rare:

  B && !B' && !!A'
  B && !B' && A'

  (0 == deco_head && 0 == next_scan && !eof)
  && (0 != deco_head' || 0 != next_scan' || eof')
  && (0 == deco_head' && 0 == next_scan' && (!eof' || 0u < scanning'))

  from A':

  0 == deco_head'
  0 == next_scan'

  (0 == deco_head && 0 == next_scan && !eof)
  && (0 || 0 || eof')
  && (0 == deco_head' && 0 == next_scan' && (!eof' || 0u < scanning'))

  (0 == deco_head && 0 == next_scan && !eof)
  && eof'
  && (0 == deco_head' && 0 == next_scan' && (!eof' || 0u < scanning'))

  For this to be true, 1 == eof' must hold.

  (0 == deco_head && 0 == next_scan && !eof)
  && eof'
  && (0 == deco_head' && 0 == next_scan' && (0 || 0u < scanning'))

  (0 == deco_head && 0 == next_scan && !eof)
  && eof'
  && (0 == deco_head' && 0 == next_scan' && (0u < scanning'))

  0 == deco_head
  && 0 == next_scan
  && !eof
  && eof'
  && 0 == deco_head'
  && 0 == next_scan'
  && 0u < scanning'

  This means, that before the change, there was nothing to decompress, nothing
  to scan, and we didn't reach EOF:

  0 == deco_head && 0 == next_scan && !eof

  and furthermore, after the change, there is still nothing to decompress,
  nothing to scan, some workers are still scanning, and we reached EOF:

  eof' && 0 == deco_head' && 0 == next_scan' && 0u < scanning'

  The key is the EOF transition. That happens only once in the lifetime of the
  process.
*/


static void
sw2w_q_init(struct sw2w_q *sw2w_q, unsigned num_worker)
{
  assert(0u < num_worker);
  xinit(&sw2w_q->proceed);
  sw2w_q->next_scan = 0;
  sw2w_q->eof = 0;
  sw2w_q->deco_head = 0;
  sw2w_q->scanning = num_worker;
}


static void
sw2w_q_uninit(struct sw2w_q *sw2w_q)
{
  assert(0u == sw2w_q->scanning);
  assert(0 == sw2w_q->deco_head);
  assert(0 != sw2w_q->eof);
  assert(0 == sw2w_q->next_scan);
  xdestroy(&sw2w_q->proceed);
}


struct w2m_blk_id
{
  struct w2w_blk_id w2w_blk_id; /* Stdin blk idx & bzip2 blk idx in former. */
  uint64_t decompr_blk_id;      /* Decompressed block for bzip2 block. */
  int last_decompr;             /* Last decompressed for bzip2 block. */
};


struct w2m_blk_nid     /* Block needed for resuming writing. */
{
  uint64_t s2w_blk_id, /* Stdin block index. */
      bzip2_blk_id,    /* Bzip2 block index within stdin block. */
      decompr_blk_id;  /* Decompressed block for bzip2 block. */
};


static int
w2m_blk_id_eq(const struct w2m_blk_id *id, const struct w2m_blk_nid *nid)
{
  return
         id->w2w_blk_id.s2w_blk_id   == nid->s2w_blk_id
      && id->w2w_blk_id.bzip2_blk_id == nid->bzip2_blk_id
      && id->decompr_blk_id          == nid->decompr_blk_id;
}


/* Worker decompression output granularity. */
#define MX_DECOMPR ((size_t)(1024u * 1024u))

struct w2m_blk                       /* Workers to muxer. */
{
  struct w2m_blk_id id;              /* Block index. */
  struct w2m_blk *next;              /* Next block in list (unordered). */
  size_t produced;                   /* Number of bytes in decompr. */
  char unsigned decompr[MX_DECOMPR]; /* Data to write to stdout. */
};


static int
w2m_blk_cmp(const void *v_a, const void *v_b)
{
  const struct w2m_blk_id *a,
      *b;

  a = &((const struct w2m_blk *)v_a)->id;
  b = &((const struct w2m_blk *)v_b)->id;

  return
        a->w2w_blk_id.s2w_blk_id   < b->w2w_blk_id.s2w_blk_id   ? -1
      : a->w2w_blk_id.s2w_blk_id   > b->w2w_blk_id.s2w_blk_id   ?  1
      : a->w2w_blk_id.bzip2_blk_id < b->w2w_blk_id.bzip2_blk_id ? -1
      : a->w2w_blk_id.bzip2_blk_id > b->w2w_blk_id.bzip2_blk_id ?  1
      : a->decompr_blk_id          < b->decompr_blk_id          ? -1
      : a->decompr_blk_id          > b->decompr_blk_id          ?  1
      : 0;
}


struct w2m_q
{
  struct cond av_or_ex_or_rel; /* New w2m_blk/s2w_blk av. or workers exited. */
  struct w2m_blk_nid needed;   /* Block needed for resuming writing. */
  struct w2m_blk *head;        /* Block list (unordered). */
  unsigned working,            /* Number of workers still running. */
      num_rel;                 /* Released s2w_blk's to return to splitter. */
};
/*
  There's something to do:
  (0 != head && list contains needed) || 0u < num_rel || 0u == working

  There's nothing to do for now (so block):
  (0 == head || list doesn't contain needed) && 0u == num_rel && 0u < working
*/


static void
w2m_q_init(struct w2m_q *w2m_q, unsigned num_worker)
{
  assert(0u < num_worker);
  xinit(&w2m_q->av_or_ex_or_rel);
  w2m_q->needed.s2w_blk_id = 0u;
  w2m_q->needed.bzip2_blk_id = 0u;
  w2m_q->needed.decompr_blk_id = 0u;
  w2m_q->head = 0;
  w2m_q->working = num_worker;
  w2m_q->num_rel = 0u;
}


static void
w2m_q_uninit(struct w2m_q *w2m_q)
{
  assert(0u == w2m_q->num_rel);
  assert(0u == w2m_q->working);
  assert(0 == w2m_q->head);
  assert(0u == w2m_q->needed.decompr_blk_id);
  assert(0u == w2m_q->needed.bzip2_blk_id);
  xdestroy(&w2m_q->av_or_ex_or_rel);
}


struct m2s_q         /* Muxer to splitter. */
{
  struct cond av;    /* Free slot available. */
  unsigned num_free; /* Number of free slots. */
};


static void
m2s_q_init(struct m2s_q *m2s_q, unsigned num_free)
{
  assert(0u < num_free);
  xinit(&m2s_q->av);
  m2s_q->num_free = num_free;
}


static void
m2s_q_uninit(struct m2s_q *m2s_q, unsigned num_free)
{
  assert(m2s_q->num_free == num_free);
  xdestroy(&m2s_q->av);
}


static void
split_chkstart(const char unsigned *comprp, size_t filled,
    struct filespec *ispec)
{
  if (filled >= sizeof intro && 0 == memcmp(comprp, intro, NUM_SHDR)) {
    comprp += NUM_SHDR;
    if (0x31u <= (unsigned)*comprp && 0x39u >= (unsigned)*comprp) {
      ++comprp;
      if (0 == memcmp(comprp, intro + NUM_SHDR + (size_t)1, NUM_BHDR)
          || 0 == memcmp(comprp, eos, NUM_BHDR)) {
        return;
      }
    }
  }

  log_fatal("%s: %s%s%s doesn't start like a bzip2 stream\n", pname,
      ispec->sep, ispec->fmt, ispec->sep);
}


static void
split(struct m2s_q *m2s_q, struct sw2w_q *sw2w_q, struct filespec *ispec)
{
  struct s2w_blk *atch_scan;
  uint64_t id;
  size_t vacant;

  atch_scan = 0;
  id = 0u;

  do {
    struct s2w_blk *s2w_blk;

    /* Grab a free slot. */
    xlock_pred(&m2s_q->av);
    while (0u == m2s_q->num_free) {
      xwait(&m2s_q->av);
    }
    --m2s_q->num_free;
    xunlock(&m2s_q->av);
    s2w_blk = xalloc(sizeof *s2w_blk);

    /* Fill block. */
    vacant = sizeof s2w_blk->compr;
    xread(ispec, s2w_blk->compr, &vacant);

    if (0u == id) {
      /*
        This check is necessary if we want to remove the input file, because
        the workers, by design, aren't offended by a missing bzip2 block header
        in a non-full first input block.
      */
      split_chkstart(s2w_blk->compr, sizeof s2w_blk->compr - vacant, ispec);
    }

    if (sizeof s2w_blk->compr == vacant) {
      /* Empty input block. */
      (*freef)(s2w_blk);
      s2w_blk = 0;
    }
    else {
      s2w_blk->id = id++;
      s2w_blk->next = 0;
      /*
        References:
          - next_decompr always,
          - current tail -> new next if not first.
      */
      s2w_blk->refno = 1u + (unsigned)(0 != atch_scan);
      s2w_blk->loaded = sizeof s2w_blk->compr - vacant;
    }

    xlock(&sw2w_q->proceed);
    assert(!sw2w_q->eof);
    /*
      (
        (0 == deco_head && 0 == next_scan && (!eof || 0u < scanning))
        && (0 != deco_head' || 0 != next_scan' || (eof' && 0u == scanning'))
      )
      || (
        (0 == deco_head && 0 == next_scan && !eof)
        && (0 != deco_head' || 0 != next_scan' || eof')
      )

      --> !eof

      (
        (0 == deco_head && 0 == next_scan && (1 || 0u < scanning))
        && (0 != deco_head' || 0 != next_scan' || (eof' && 0u == scanning'))
      )
      || (
        (0 == deco_head && 0 == next_scan && 1)
        && (0 != deco_head' || 0 != next_scan' || eof')
      )

      (
        (0 == deco_head && 0 == next_scan)
        && (0 != deco_head' || 0 != next_scan' || (eof' && 0u == scanning'))
      )
      || (
        (0 == deco_head && 0 == next_scan)
        && (0 != deco_head' || 0 != next_scan' || eof')
      )
    */
    if (0 == sw2w_q->next_scan) {
      sw2w_q->next_scan = s2w_blk;

      /*
        --> 0 == next_scan
        --> 0 != next_scan' || eof'

        (
          (0 == deco_head && 1)
          && (0 != deco_head' || 0 != next_scan' || (1 && 0u == scanning'))
        )
        || (
          (0 == deco_head && 1)
          && (0 != deco_head' || 1)
        )

        (
          (0 == deco_head)
          && (0 != deco_head' || 0 != next_scan' || (0u == scanning'))
        )
        || (0 == deco_head)

        (0 == deco_head)
        && (
          (0 != deco_head' || 0 != next_scan' || (0u == scanning'))
          || 1
        )

        (0 == deco_head)
      */
      if (0 == sw2w_q->deco_head) {
        xbroadcast(&sw2w_q->proceed);
      }
    }
    /*
      Otherwise:
      --> 0 == (0 == next_scan)

      (
        (0 == deco_head && 0)
        && (0 != deco_head' || 0 != next_scan' || (eof' && 0u == scanning'))
      )
      || (
        (0 == deco_head && 0)
        && (0 != deco_head' || 0 != next_scan' || eof')
      )

      0
    */

    if (0 != atch_scan) {
      assert(0u < atch_scan->refno);
      atch_scan->next = s2w_blk;
    }

    if (0u == vacant) {
      xunlock(&sw2w_q->proceed);
      atch_scan = s2w_blk;
    }
    else {
      sw2w_q->eof = 1;
      xunlock(&sw2w_q->proceed);
    }
  } while (0u == vacant);

  if (sizeof atch_scan->compr == vacant) {
    xlock(&m2s_q->av);
    ++m2s_q->num_free;
    xunlock(&m2s_q->av);
  }
}


struct split_arg
{
  struct m2s_q *m2s_q;
  struct sw2w_q *sw2w_q;
  struct filespec *ispec;
};


static void *
split_wrap(void *v_split_arg)
{
  struct split_arg *split_arg;

  split_arg = v_split_arg;

  split(split_arg->m2s_q, split_arg->sw2w_q, split_arg->ispec);
  return 0;
}


static void
work_compl(struct w2w_blk *w2w_blk, struct filespec *ispec)
{
  unsigned fb,
      ub,
      save_bitbuf;
  char unsigned eos_crc[sizeof eos + NUM_CRC];

  assert(w2w_blk->reconstructed <= sizeof w2w_blk->streamdata);

  if (w2w_blk->reconstructed < sizeof intro + NUM_CRC + sizeof eos) {
    log_fatal("%s: %s%s%s: compressed block too short\n", pname, ispec->sep,
        ispec->fmt, ispec->sep);
  }

  (void)memcpy(w2w_blk->streamdata, intro, sizeof intro);

  fb = w2w_blk->rbits_left;
  ub = CHAR_BIT - fb;

  w2w_blk->reconstructed -= sizeof eos;
  if (0u < ub) {
    save_bitbuf = (unsigned)w2w_blk->streamdata[w2w_blk->reconstructed] >> fb;
  }

  if (sizeof w2w_blk->streamdata - w2w_blk->reconstructed
      < sizeof eos_crc + (size_t)(0u < ub)) {
    log_fatal("%s: %s%s%s: compressed block too long\n", pname, ispec->sep,
        ispec->fmt, ispec->sep);
  }

  if (0u < ub) {
    size_t ctr;

    (void)memcpy(eos_crc, eos, sizeof eos);
    (void)memcpy(eos_crc + sizeof eos, w2w_blk->streamdata + sizeof intro,
        NUM_CRC);

    w2w_blk->streamdata[w2w_blk->reconstructed++]
        = save_bitbuf << fb | (unsigned)eos_crc[0u] >> ub;
    for (ctr = 1u; ctr < sizeof eos_crc; ++ctr) {
      w2w_blk->streamdata[w2w_blk->reconstructed++]
          = (unsigned)eos_crc[ctr - 1u] << fb | (unsigned)eos_crc[ctr] >> ub;
    }
    w2w_blk->streamdata[w2w_blk->reconstructed++]
        = (unsigned)eos_crc[sizeof eos_crc - 1u] << fb;
  }
  else {
    (void)memcpy(w2w_blk->streamdata + w2w_blk->reconstructed, eos,
        sizeof eos);
    w2w_blk->reconstructed += sizeof eos;
    (void)memcpy(w2w_blk->streamdata + w2w_blk->reconstructed,
        w2w_blk->streamdata + sizeof intro, NUM_CRC);
    w2w_blk->reconstructed += NUM_CRC;
  }
}


static void
work_decompr(struct w2w_blk *w2w_blk, struct w2m_q *w2m_q,
    struct filespec *ispec)
{
  int ybret;
  uint64_t decompr_blk_id;
  YBibs_t *ibs;
  YBdec_t *dec;
  void *ibuf, *obuf;
  size_t ileft, oleft;

  decompr_blk_id = 0u;

  ibs = YBibs_init();
  dec = YBdec_init();

  ibuf = w2w_blk->streamdata;
  ileft = w2w_blk->reconstructed;
  ybret = YBibs_retrieve(ibs, dec, ibuf, &ileft);
  if (ybret == YB_UNDERFLOW)
    log_fatal("%s: %s%s%s: misrecognized a bit-sequence as a block"
        " delimiter\n", pname, ispec->sep, ispec->fmt, ispec->sep);
  if (ybret != YB_OK)
    log_fatal("%s: %s%s%s: data error while retrieving block: %s\n",
        pname, ispec->sep, ispec->fmt, ispec->sep, YBerr_detail(ybret));

  ybret = YBdec_work(dec);
  if (ybret != YB_OK)
    log_fatal("%s: %s%s%s: data error while decompressing block: %s\n",
        pname, ispec->sep, ispec->fmt, ispec->sep, YBerr_detail(ybret));

  do {
    struct w2m_blk *w2m_blk;

    w2m_blk = xalloc(sizeof *w2m_blk);

    obuf = w2m_blk->decompr;
    oleft = sizeof w2m_blk->decompr;
    ybret = YBdec_emit(dec, obuf, &oleft);
    if (ybret == YB_OK) {
      YBdec_join(dec);
      ybret = YBibs_check(ibs);
      assert(ybret == YB_OK);
    }
    else if (ybret != YB_UNDERFLOW)
      log_fatal("%s: %s%s%s: data error while emitting block: %s\n",
          pname, ispec->sep, ispec->fmt, ispec->sep, YBerr_detail(ybret));

    w2m_blk->id.w2w_blk_id = w2w_blk->id;
    w2m_blk->id.decompr_blk_id = decompr_blk_id++;
    w2m_blk->id.last_decompr = (ybret == YB_OK);
    w2m_blk->produced = sizeof w2m_blk->decompr - oleft;

    /*
      Push decompressed sub-block to muxer.

      If the muxer blocks, then this is true before the changes below:
      (if this is true before the changes, then the muxer may be blocking:)
      (0 == head || list doesn't cont. needed) && 0u == num_rel && 0u < working
      (0 == head || list doesn't cont. needed) && 0u == num_rel && 1
      (0 == head || list doesn't cont. needed) && 0u == num_rel

      Furthermore, if this is true after the changes, it must be woken up:
      (0 != head' && list' contains needed) || 0u < num_rel' || 0u == working'
      (1          && list' contains needed) || 0u < num_rel  || 0u == working
                     list' contains needed  || 0             || 0
                     list' contains needed

      Full condition for wakeup:
      (0 == head || list doesn't cont. needed)
          && 0u == num_rel && list' contains needed

      We don't know (0 == head || list doesn't cont. needed), assume it's 1:
      1 && 0u == num_rel && list' contains needed
           0u == num_rel && list' contains needed

      Using the previous assumption,
           0u == num_rel && (list' == list + needed)
    */
    xlock(&w2m_q->av_or_ex_or_rel);
    assert(0u < w2m_q->working);
    w2m_blk->next = w2m_q->head;
    w2m_q->head = w2m_blk;
    if (0u == w2m_q->num_rel && w2m_blk_id_eq(&w2m_blk->id, &w2m_q->needed)) {
      xsignal(&w2m_q->av_or_ex_or_rel);
    }
    xunlock(&w2m_q->av_or_ex_or_rel);
  } while (ybret == YB_UNDERFLOW);

  YBdec_destroy(dec);
  YBibs_destroy(ibs);
}


static void
work_oflush(struct w2w_blk **p_w2w_blk, uint64_t s2w_blk_id,
    uint64_t *bzip2_blk_id, int last_bzip2, struct sw2w_q *sw2w_q)
{
  struct w2w_blk *w2w_blk;

  w2w_blk = *p_w2w_blk;

  w2w_blk->id.s2w_blk_id = s2w_blk_id;
  w2w_blk->id.bzip2_blk_id = (*bzip2_blk_id)++;
  w2w_blk->id.last_bzip2 = last_bzip2;

  /* Push mostly reconstructed bzip2 stream to workers. */
  xlock(&sw2w_q->proceed);
  assert(0u < sw2w_q->scanning);
  /*
    (
      (0 == deco_head && 0 == next_scan && (!eof || 0u < scanning))
      && (0 != deco_head' || 0 != next_scan' || (eof' && 0u == scanning'))
    )
    || (
      (0 == deco_head && 0 == next_scan && !eof)
      && (0 != deco_head' || 0 != next_scan' || eof')
    )

    --> 1 == (0u < scanning),
    --> scanning' = scanning, 0 == (0u == scanning')

    (
      (0 == deco_head && 0 == next_scan && (!eof || 1))
      && (0 != deco_head' || 0 != next_scan' || (eof' && 0))
    )
    || (
      (0 == deco_head && 0 == next_scan && !eof)
      && (0 != deco_head' || 0 != next_scan' || eof')
    )

    (
      (0 == deco_head && 0 == next_scan)
      && (0 != deco_head' || 0 != next_scan')
    )
    || (
      (0 == deco_head && 0 == next_scan && !eof)
      && (0 != deco_head' || 0 != next_scan' || eof')
    )

    (0 == deco_head && 0 == next_scan)
    && (
      (0 != deco_head' || 0 != next_scan')
      || (!eof && (0 != deco_head' || 0 != next_scan' || eof'))
    )

    --> next_scan' = next_scan
    --> eof' = eof
    --> 1 == (0 != deco_head')

    (0 == deco_head && 0 == next_scan)
    && (
      (1 || 0 != next_scan)
      || (!eof && (1 || 0 != next_scan || eof))
    )

    0 == deco_head && 0 == next_scan
  */
  if (0 == sw2w_q->deco_head && 0 == sw2w_q->next_scan) {
    xbroadcast(&sw2w_q->proceed);
  }
  w2w_blk->next = sw2w_q->deco_head;
  sw2w_q->deco_head = w2w_blk;
  xunlock(&sw2w_q->proceed);

  *p_w2w_blk = 0;
}


static struct s2w_blk *
work_get_first(struct sw2w_q *sw2w_q, struct w2m_q *w2m_q,
    struct filespec *ispec)
{
  /* Hold "xlock_pred(&sw2w_q->proceed)" on entry. Will hold on return. */

  int loop;

  loop = 0;
  assert(0u < sw2w_q->scanning);

  --sw2w_q->scanning;
  for (;;) {
    /* Decompression enjoys absolute priority over scanning. */
    if (0 != sw2w_q->deco_head) {
      /*
        (
          (0 == deco_head && 0 == next_scan && (!eof || 0u < scanning))
          && (0 != deco_head' || 0 != next_scan' || (eof' && 0u == scanning'))
        )
        || (
          (0 == deco_head && 0 == next_scan && !eof)
          && (0 != deco_head' || 0 != next_scan' || eof')
        )

        --> 0 == (0 == deco_head)

        (
          (0 && 0 == next_scan && (!eof || 0u < scanning))
          && (0 != deco_head' || 0 != next_scan' || (eof' && 0u == scanning'))
        )
        || (
          (0 && 0 == next_scan && !eof)
          && (0 != deco_head' || 0 != next_scan' || eof')
        )

        0
      */
      struct w2w_blk *deco;

      deco = sw2w_q->deco_head;
      sw2w_q->deco_head = deco->next;
      xunlock(&sw2w_q->proceed);

      work_compl(deco, ispec);
      work_decompr(deco, w2m_q, ispec);
      (*freef)(deco);

      xlock_pred(&sw2w_q->proceed);
    }
    else {
      if (0 != sw2w_q->next_scan) {
        /*
          (
            (0 == deco_head && 0 == next_scan && (!eof || 0u < scanning))
            && (
              0 != deco_head' || 0 != next_scan' || (eof' && 0u == scanning')
            )
          )
          || (
            (0 == deco_head && 0 == next_scan && !eof)
            && (0 != deco_head' || 0 != next_scan' || eof')
          )

          --> 0 == (0 == next_scan)

          (
            (0 == deco_head && 0 && (!eof || 0u < scanning))
            && (
              0 != deco_head' || 0 != next_scan' || (eof' && 0u == scanning')
            )
          )
          || (
            (0 == deco_head && 0 && !eof)
            && (0 != deco_head' || 0 != next_scan' || eof')
          )

          0
        */
        ++sw2w_q->scanning;
        return sw2w_q->next_scan;
      }

      if (sw2w_q->eof && 0u == sw2w_q->scanning) {
        /*
          (
            (0 == deco_head && 0 == next_scan && (!eof || 0u < scanning))
            && (
              0 != deco_head' || 0 != next_scan' || (eof' && 0u == scanning')
            )
          )
          || (
            (0 == deco_head && 0 == next_scan && !eof)
            && (0 != deco_head' || 0 != next_scan' || eof')
          )

          -->                          1 == (0 == deco_head)
          --> deco_head' == deco_head, 0 == (0 != deco_head')
          -->                          1 == (0 == next_scan)
          --> next_scan' == next_scan, 0 == (0 != next_scan')
          -->              1 == eof
          --> eof' == eof, 1 == eof'

          (
            (1 && 1 && (0 || 0u < scanning))
            && (
              0 || 0 || (1 && 0u == scanning')
            )
          )
          || (
            (1 && 1 && 0)
            && (0 || 0 || 1)
          )

          (0u < scanning) && (0u == scanning')

          --> 1 == (0u == scanning')

          (0u < scanning) && (1)

          0u < scanning

          ---o---

          --> The following statement is always true.

          (1 == loop && scanning' == scanning) || (0 == loop && 0u < scanning)

          --> 0u == scanning'

          (1 == loop && 0u == scanning) || (0 == loop && 0u < scanning)

          (0 == loop) <=> (0u < scanning)
        */
        if (0 == loop) {
          xbroadcast(&sw2w_q->proceed);
        }
        return 0;
      }

      /*
        (
          (0 == deco_head && 0 == next_scan && (!eof || 0u < scanning))
          && (0 != deco_head' || 0 != next_scan' || (eof' && 0u == scanning'))
        )
        || (
          (0 == deco_head && 0 == next_scan && !eof)
          && (0 != deco_head' || 0 != next_scan' || eof')
        )

        --> 1 == (0 == deco_head)
        --> 1 == (0 == next_scan)
        --> 0 == (eof && 0u == scanning')
        --> eof == eof', 0 == (eof' && 0u == scanning')
        --> deco_head == deco_head', 0 == (0 != deco_head')
        --> next_scan == next_scan', 0 == (0 != next_scan')

        (
          (1 && 1 && (!eof || 0u < scanning))
          && (0 || 0 || (0))
        )
        || (
          (1 && 1 && !eof)
          && (0 || 0 || eof')
        )

        !eof && eof'

        --> eof == eof'

        !eof && eof

        0
      */
      xwait(&sw2w_q->proceed);
    }

    loop = 1;
  }
}


static void
work_release(struct s2w_blk *s2w_blk, struct sw2w_q *sw2w_q,
    struct w2m_q *w2m_q)
{
  assert(0u < s2w_blk->refno);
  if (0u == --s2w_blk->refno) {
    assert(s2w_blk != sw2w_q->next_scan);
    xunlock(&sw2w_q->proceed);
    (*freef)(s2w_blk);
    xlock(&w2m_q->av_or_ex_or_rel);
    if (0u == w2m_q->num_rel++) {
      xsignal(&w2m_q->av_or_ex_or_rel);
    }
    xunlock(&w2m_q->av_or_ex_or_rel);
  }
  else {
    xunlock(&sw2w_q->proceed);
  }
}


static struct s2w_blk *
work_get_second(struct s2w_blk *s2w_blk, struct sw2w_q *sw2w_q,
    struct w2m_q *w2m_q, struct filespec *ispec)
{
  xlock_pred(&sw2w_q->proceed);

  for (;;) {
    /* Decompression enjoys absolute priority over scanning. */
    if (0 != sw2w_q->deco_head) {
      struct w2w_blk *deco;

      deco = sw2w_q->deco_head;
      sw2w_q->deco_head = deco->next;
      xunlock(&sw2w_q->proceed);

      work_compl(deco, ispec);
      work_decompr(deco, w2m_q, ispec);
      (*freef)(deco);

      xlock_pred(&sw2w_q->proceed);
    }
    else {
      if (0 != sw2w_q->next_scan || sw2w_q->eof) {
        struct s2w_blk *next;

        assert(0 == sw2w_q->next_scan || 0 != s2w_blk->next);
        /*
          If "next_scan" is non-NULL: "next_scan" became a pointer to the
          current first element to scan either by assuming the values of the
          "next" pointers of the elements that were once pointed to by
          "next_scan" (including the one we're now trying to get the next one
          for), or by being updated by the splitter, but then the splitter
          updated "atch_scan->next" too. Thus no such "next" pointer can be 0.
          Also, "next_scan" becomes non-NULL no later than "s2w_blk->next",
          see split().

          If "next_scan" is NULL and so we're here because the splitter hit
          EOF: we'll return NULL iff we're trying to get the next input block
          for the last input block.
        */

        next = s2w_blk->next;
        work_release(s2w_blk, sw2w_q, w2m_q);
        return next;
      }

      xwait(&sw2w_q->proceed);
    }
  }
}


static void
work(struct sw2w_q *sw2w_q, struct w2m_q *w2m_q, struct filespec *ispec)
{
  struct s2w_blk *s2w_blk;
  uint64_t first_s2w_blk_id;
  unsigned ibitbuf,
      ibits_left;
  size_t ipos;

  uint64_t bzip2_blk_id;

  struct w2w_blk *w2w_blk;
  unsigned rbitbuf;

  unsigned bit;
  uint64_t search;

again:
  xlock_pred(&sw2w_q->proceed);
  s2w_blk = work_get_first(sw2w_q, w2m_q, ispec);
  if (0 == s2w_blk) {
    xunlock(&sw2w_q->proceed);

    /* Notify muxer when last worker exits. */
    xlock(&w2m_q->av_or_ex_or_rel);
    if (0u == --w2m_q->working && 0u == w2m_q->num_rel && 0 == w2m_q->head) {
      xsignal(&w2m_q->av_or_ex_or_rel);
    }
    xunlock(&w2m_q->av_or_ex_or_rel);
    return;
  }
  sw2w_q->next_scan = s2w_blk->next;
  xunlock(&sw2w_q->proceed);

  first_s2w_blk_id = s2w_blk->id;
  ibits_left = 0u;
  ipos = 0u;
  assert(ipos < s2w_blk->loaded);

  bzip2_blk_id = 0u;
  w2w_blk = 0;
  search = 0u;

  do {  /* never seen magic */
    if (0 == ibits_left) {
      if (s2w_blk->loaded == ipos) {
        if (sizeof s2w_blk->compr == s2w_blk->loaded) {
          log_fatal("%s: %s%s%s: missing bzip2 block header in full first"
              " input block\n", pname, ispec->sep, ispec->fmt, ispec->sep);
        }

        /* Short first input block without a bzip2 block header. */
        assert(sizeof s2w_blk->compr > s2w_blk->loaded);

        xlock(&sw2w_q->proceed);
        assert(0 == s2w_blk->next);
        assert(sw2w_q->eof);
        work_release(s2w_blk, sw2w_q, w2m_q);

        assert(0 == w2w_blk);
        goto again;
      }

      ibitbuf = s2w_blk->compr[ipos++];
      ibits_left = CHAR_BIT;
    }

    bit = ibitbuf >> --ibits_left & 1u;
    search = (search << 1 | bit) & magic_mask;
  } while (magic_hdr != search);  /* never seen magic */

  w2w_blk = xalloc(sizeof *w2w_blk);
  w2w_blk->reconstructed = sizeof intro;
  w2w_blk->rbits_left = CHAR_BIT;
  rbitbuf = 0u;

  for (;;) {  /* first block */
    do {  /* in bzip2 */
      if (0 == ibits_left) {
        if (s2w_blk->loaded == ipos) {
          if (sizeof s2w_blk->compr > s2w_blk->loaded) {
            log_fatal("%s: %s%s%s: unterminated bzip2 block in short first"
                " input block\n", pname, ispec->sep, ispec->fmt, ispec->sep);
          }
          assert(sizeof s2w_blk->compr == s2w_blk->loaded);

          s2w_blk = work_get_second(s2w_blk, sw2w_q, w2m_q, ispec);

          if (0 == s2w_blk) {
            log_fatal("%s: %s%s%s: unterminated bzip2 block in full first"
                " input block\n", pname, ispec->sep, ispec->fmt, ispec->sep);
          }

          ipos = 0u;
          assert(ipos < s2w_blk->loaded);
          goto in_second;
        }

        ibitbuf = s2w_blk->compr[ipos++];
        ibits_left = CHAR_BIT;
      }

      bit = ibitbuf >> --ibits_left & 1u;
      search = (search << 1 | bit) & magic_mask;

      /* Push bit to bzip2 block being reconstructed. */
      rbitbuf = rbitbuf << 1 | bit;
      if (0u == --w2w_blk->rbits_left) {
        if (sizeof w2w_blk->streamdata == w2w_blk->reconstructed) {
          log_fatal("%s: %s%s%s: compressed block too long\n", pname,
              ispec->sep, ispec->fmt, ispec->sep);
        }
        w2w_blk->streamdata[w2w_blk->reconstructed++] = rbitbuf;
        w2w_blk->rbits_left = CHAR_BIT;
      }

      if (magic_hdr == search) {
        work_oflush(&w2w_blk, first_s2w_blk_id, &bzip2_blk_id, 0, sw2w_q);

        w2w_blk = xalloc(sizeof *w2w_blk);
        w2w_blk->reconstructed = sizeof intro;
        w2w_blk->rbits_left = CHAR_BIT;
        rbitbuf = 0u;
      }
    } while (magic_eos != search);  /* in bzip2 */

    do {  /* out bzip2 */
      if (0 == ibits_left) {
        if (s2w_blk->loaded == ipos) {
          if (sizeof s2w_blk->compr > s2w_blk->loaded) {
            xlock(&sw2w_q->proceed);
            assert(0 == s2w_blk->next);
            assert(sw2w_q->eof);
            work_release(s2w_blk, sw2w_q, w2m_q);
            s2w_blk = 0;
          }
          else {
            assert(sizeof s2w_blk->compr == s2w_blk->loaded);
            s2w_blk = work_get_second(s2w_blk, sw2w_q, w2m_q, ispec);
          }

          if (0 == s2w_blk) {
            work_oflush(&w2w_blk, first_s2w_blk_id, &bzip2_blk_id, 1, sw2w_q);
            goto again;
          }

          ipos = 0u;
          assert(ipos < s2w_blk->loaded);
          goto out_second;
        }

        ibitbuf = s2w_blk->compr[ipos++];
        ibits_left = CHAR_BIT;
      }

      bit = ibitbuf >> --ibits_left & 1u;
      search = (search << 1 | bit) & magic_mask;
    } while (magic_hdr != search);  /* out bzip2 */

    work_oflush(&w2w_blk, first_s2w_blk_id, &bzip2_blk_id, 0, sw2w_q);

    w2w_blk = xalloc(sizeof *w2w_blk);
    w2w_blk->reconstructed = sizeof intro;
    w2w_blk->rbits_left = CHAR_BIT;
    rbitbuf = 0u;
  }  /* first block */
 
  for (;;) {  /* second block */
    do {  /* in bzip2 */
      if (0 == ibits_left) {
        if (s2w_blk->loaded == ipos) {
          log_fatal("%s: %s%s%s: %s second input block\n",
              sizeof s2w_blk->compr == s2w_blk->loaded ?
              "missing bzip2 block header in full" :
              "unterminated bzip2 block in short",
              pname, ispec->sep, ispec->fmt, ispec->sep);
          }

      in_second:
        ibitbuf = s2w_blk->compr[ipos++];
        ibits_left = CHAR_BIT;
      }

      bit = ibitbuf >> --ibits_left & 1u;
      search = (search << 1 | bit) & magic_mask;

      /* Push bit to bzip2 block being reconstructed. */
      rbitbuf = rbitbuf << 1 | bit;
      if (0u == --w2w_blk->rbits_left) {
        if (sizeof w2w_blk->streamdata == w2w_blk->reconstructed) {
          log_fatal("%s: %s%s%s: compressed block too long\n", pname,
              ispec->sep, ispec->fmt, ispec->sep);
        }
        w2w_blk->streamdata[w2w_blk->reconstructed++] = rbitbuf;
        w2w_blk->rbits_left = CHAR_BIT;
      }

      if (magic_hdr == search) {
        if (sizeof eos + (size_t)(0u < ibits_left) <= ipos) {
          xlock(&sw2w_q->proceed);
          work_release(s2w_blk, sw2w_q, w2m_q);
          work_oflush(&w2w_blk, first_s2w_blk_id, &bzip2_blk_id, 1, sw2w_q);
          goto again;
        }

        work_oflush(&w2w_blk, first_s2w_blk_id, &bzip2_blk_id, 0, sw2w_q);

        w2w_blk = xalloc(sizeof *w2w_blk);
        w2w_blk->reconstructed = sizeof intro;
        w2w_blk->rbits_left = CHAR_BIT;
        rbitbuf = 0u;
      }
    } while (magic_eos != search);  /* in bzip2 */

    do {  /* out bzip2 */
      if (0 == ibits_left) {
        if (s2w_blk->loaded == ipos) {
          if (sizeof s2w_blk->compr == s2w_blk->loaded) {
            log_fatal("%s: %s%s%s: missing bzip2 block header in full"
                " second input block\n", pname, ispec->sep, ispec->fmt,
                ispec->sep);
          }

          /* Terminated bzip2 block at end of short second input block. */
          xlock(&sw2w_q->proceed);
          assert(0 == s2w_blk->next);
          assert(sw2w_q->eof);
          work_release(s2w_blk, sw2w_q, w2m_q);

          work_oflush(&w2w_blk, first_s2w_blk_id, &bzip2_blk_id, 1, sw2w_q);
          goto again;
        }

      out_second:
        ibitbuf = s2w_blk->compr[ipos++];
        ibits_left = CHAR_BIT;
      }

      bit = ibitbuf >> --ibits_left & 1u;
      search = (search << 1 | bit) & magic_mask;

    } while (magic_hdr != search);  /* out bzip2 */

    if (sizeof eos + (size_t)(0u < ibits_left) <= ipos) {
      xlock(&sw2w_q->proceed);
      work_release(s2w_blk, sw2w_q, w2m_q);
      work_oflush(&w2w_blk, first_s2w_blk_id, &bzip2_blk_id, 1, sw2w_q);
      goto again;
    }

    work_oflush(&w2w_blk, first_s2w_blk_id, &bzip2_blk_id, 0, sw2w_q);

    w2w_blk = xalloc(sizeof *w2w_blk);
    w2w_blk->reconstructed = sizeof intro;
    w2w_blk->rbits_left = CHAR_BIT;
    rbitbuf = 0u;
  }  /* second block */
}


struct work_arg
{
  struct sw2w_q *sw2w_q;
  struct w2m_q *w2m_q;
  struct filespec *ispec;
};


static void *
work_wrap(void *v_work_arg)
{
  struct work_arg *work_arg;

  work_arg = v_work_arg;
  work(work_arg->sw2w_q, work_arg->w2m_q, work_arg->ispec);
  return 0;
}


static void *
reord_alloc(size_t size, void *ignored)
{
  return xalloc(size);
}


static void
reord_dealloc(void *ptr, void *ignored)
{
  (*freef)(ptr);
}


static void
mux_write(struct lacos_rbtree_node **reord, struct w2m_blk_nid *reord_needed,
    struct filespec *ospec)
{
  assert(0 != *reord);

  /*
    Go on until the tree becomes empty or the next sub-block is found to be
    missing.
  */
  do {
    struct lacos_rbtree_node *reord_head;
    struct w2m_blk *reord_w2m_blk;

    reord_head = lacos_rbtree_min(*reord);
    assert(0 != reord_head);

    reord_w2m_blk = *(void **)reord_head;
    if (!w2m_blk_id_eq(&reord_w2m_blk->id, reord_needed)) {
      break;
    }

    /* Write out "reord_w2m_blk". */
    xwrite(ospec, reord_w2m_blk->decompr, reord_w2m_blk->produced);

    if (reord_w2m_blk->id.last_decompr) {
      if (reord_w2m_blk->id.w2w_blk_id.last_bzip2) {
        ++reord_needed->s2w_blk_id;
        reord_needed->bzip2_blk_id = 0u;
      }
      else {
        ++reord_needed->bzip2_blk_id;
      }

      reord_needed->decompr_blk_id = 0u;
    }
    else {
      ++reord_needed->decompr_blk_id;
    }

    lacos_rbtree_delete(
        reord,         /* new_root */
        reord_head,    /* old_node */
        0,             /* old_data */
        reord_dealloc, /* dealloc() */
        0              /* alloc_ctl */
    );

    /* Release "reord_w2m_blk". */
    (*freef)(reord_w2m_blk);
  } while (0 != *reord);
}


static void
mux(struct w2m_q *w2m_q, struct m2s_q *m2s_q, struct filespec *ospec)
{
  struct lacos_rbtree_node *reord;
  struct w2m_blk_nid reord_needed;
  unsigned working;

  reord = 0;
  reord_needed.s2w_blk_id = 0u;
  reord_needed.bzip2_blk_id = 0u;
  reord_needed.decompr_blk_id = 0u;

  xlock_pred(&w2m_q->av_or_ex_or_rel);
  do {
    struct w2m_blk *w2m_blk;
    unsigned num_rel;

    for (;;) {
      w2m_blk = w2m_q->head;
      working = w2m_q->working;
      num_rel = w2m_q->num_rel;

      if (0 != w2m_blk || 0u == working || 0u < num_rel) {
        break;
      }
      xwait(&w2m_q->av_or_ex_or_rel);
    }

    w2m_q->head = 0;
    w2m_q->num_rel = 0u;
    xunlock(&w2m_q->av_or_ex_or_rel);

    if (0u < num_rel) {
      xlock(&m2s_q->av);
      if (0u == m2s_q->num_free) {
        xsignal(&m2s_q->av);
      }
      m2s_q->num_free += num_rel;
      xunlock(&m2s_q->av);
    }

    if (0 != w2m_blk) {
      /* Merge sub-blocks fetched this time into tree. */
      do {
        int tmp;
        struct lacos_rbtree_node *new_node;
        struct w2m_blk *next;

        tmp = lacos_rbtree_insert(
            &reord,      /* new_root */
            &new_node,   /* new_node */
            w2m_blk,     /* new_data */
            w2m_blk_cmp, /* cmp() */
            reord_alloc, /* alloc() */
            0            /* alloc_ctl */
        );
        /*
          w2m_blk_id triplet collision shouldn't happen, and see reord_alloc()
          too.
        */
        assert(0 == tmp);

        next = w2m_blk->next;
        w2m_blk->next = 0;
        w2m_blk = next;
      } while (0 != w2m_blk);

      /* Write out initial continuous sequence of reordered sub-blocks. */
      mux_write(&reord, &reord_needed, ospec);
    }

    if (0u == working) {
      xlock(&w2m_q->av_or_ex_or_rel);
    }
    else {
      xlock_pred(&w2m_q->av_or_ex_or_rel);
    }

    w2m_q->needed = reord_needed;
  } while (0u < working);
  xunlock(&w2m_q->av_or_ex_or_rel);

  assert(0u == reord_needed.decompr_blk_id);
  assert(0u == reord_needed.bzip2_blk_id);
  assert(0 == reord);
}


static void
lbunzip2(unsigned num_worker, unsigned num_slot, int print_cctrs,
    struct filespec *ispec, struct filespec *ospec)
{
  struct sw2w_q sw2w_q;
  struct w2m_q w2m_q;
  struct m2s_q m2s_q;
  struct split_arg split_arg;
  pthread_t splitter;
  struct work_arg work_arg;
  pthread_t *worker;
  unsigned i;

  sw2w_q_init(&sw2w_q, num_worker);
  w2m_q_init(&w2m_q, num_worker);
  m2s_q_init(&m2s_q, num_slot);

  split_arg.m2s_q = &m2s_q;
  split_arg.sw2w_q = &sw2w_q;
  split_arg.ispec = ispec;
  xcreate(&splitter, split_wrap, &split_arg);

  work_arg.sw2w_q = &sw2w_q;
  work_arg.w2m_q = &w2m_q;
  work_arg.ispec = ispec;

  assert(0u < num_worker);
  assert(SIZE_MAX / sizeof *worker >= num_worker);
  worker = xalloc(num_worker * sizeof *worker);
  for (i = 0u; i < num_worker; ++i) {
    xcreate(&worker[i], work_wrap, &work_arg);
  }

  mux(&w2m_q, &m2s_q, ospec);

  i = num_worker;
  do {
    xjoin(worker[--i]);
  } while (0u < i);
  (*freef)(worker);

  xjoin(splitter);

  if (print_cctrs) {
    log_info(
        "%s: %s%s%s: condvar counters:\n"
#define FW ((int)sizeof(long unsigned) * (int)CHAR_BIT / 3 + 1)
        "%s: any worker tried to consume from splitter or workers: %*lu\n"
        "%s: any worker stalled                                  : %*lu\n"
        "%s: muxer tried to consume from workers                 : %*lu\n"
        "%s: muxer stalled                                       : %*lu\n"
        "%s: splitter tried to consume from muxer                : %*lu\n"
        "%s: splitter stalled                                    : %*lu\n",
        pname, ispec->sep, ispec->fmt, ispec->sep,
        pname, FW, sw2w_q.proceed.ccount,
        pname, FW, sw2w_q.proceed.wcount,
        pname, FW, w2m_q.av_or_ex_or_rel.ccount,
        pname, FW, w2m_q.av_or_ex_or_rel.wcount,
        pname, FW, m2s_q.av.ccount,
        pname, FW, m2s_q.av.wcount
#undef FW
    );
  }

  m2s_q_uninit(&m2s_q, num_slot);
  w2m_q_uninit(&w2m_q);
  sw2w_q_uninit(&sw2w_q);
}


void *
lbunzip2_wrap(void *v_lbunzip2_arg)
{
  struct lbunzip2_arg *lbunzip2_arg;

  lbunzip2_arg = v_lbunzip2_arg;
  lbunzip2(
      lbunzip2_arg->num_worker,
      lbunzip2_arg->num_slot,
      lbunzip2_arg->print_cctrs,
      lbunzip2_arg->ispec,
      lbunzip2_arg->ospec
  );

  xraise(SIGUSR2);
  return 0;
}
