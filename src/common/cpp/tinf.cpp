/*
 * tinf - tiny inflate library (RFC 1951)
 * Clean, safe C++ implementation of raw DEFLATE decompression.
 */

#include "krypton/updater/tinf.h"

#include <cstring>

namespace {

struct TinfData {
  const unsigned char* source;
  unsigned int sourceLen;
  unsigned int sourcePos;
  unsigned int tag;
  int bitcount;

  unsigned char* dest;
  unsigned int destLen;
  unsigned int destPos;
};

struct TinfTree {
  unsigned short counts[16];
  unsigned short symbols[288];
};

unsigned int tinf_getbit(TinfData* d) {
  if (d->bitcount == 0) {
    if (d->sourcePos < d->sourceLen) {
      d->tag = d->source[d->sourcePos++];
    } else {
      d->tag = 0;
    }
    d->bitcount = 8;
  }
  unsigned int bit = d->tag & 1;
  d->tag >>= 1;
  d->bitcount--;
  return bit;
}

unsigned int tinf_read_bits(TinfData* d, int num, int base) {
  unsigned int val = 0;
  if (num > 0) {
    for (int i = 0; i < num; ++i) {
      val |= (tinf_getbit(d) << i);
    }
  }
  return base + val;
}

void tinf_build_tree(TinfTree* t, const unsigned char* lengths, unsigned int num) {
  unsigned short offsets[16];
  for (int i = 0; i < 16; ++i) t->counts[i] = 0;
  for (unsigned int i = 0; i < num; ++i) t->counts[lengths[i]]++;
  t->counts[0] = 0;

  unsigned int offs = 0;
  for (int i = 1; i < 16; ++i) {
    offsets[i] = static_cast<unsigned short>(offs);
    offs += t->counts[i];
  }

  for (unsigned int i = 0; i < num; ++i) {
    if (lengths[i] != 0) {
      t->symbols[offsets[lengths[i]]++] = static_cast<unsigned short>(i);
    }
  }
}

int tinf_decode_symbol(TinfData* d, const TinfTree* t) {
  int code = 0;
  int count = 0;
  int first = 0;
  int index = 0;

  for (int len = 1; len <= 15; ++len) {
    code |= tinf_getbit(d);
    count = t->counts[len];
    if (code - count < first) {
      return t->symbols[index + (code - first)];
    }
    index += count;
    first += count;
    first <<= 1;
    code <<= 1;
  }
  return TINF_DATA_ERROR;
}

int tinf_inflate_uncompressed_block(TinfData* d) {
  d->bitcount = 0;
  if (d->sourcePos + 4 > d->sourceLen) return TINF_DATA_ERROR;
  unsigned int length = d->source[d->sourcePos] | (d->source[d->sourcePos + 1] << 8);
  unsigned int invlength = d->source[d->sourcePos + 2] | (d->source[d->sourcePos + 3] << 8);
  d->sourcePos += 4;
  if (length != (~invlength & 0xFFFF)) return TINF_DATA_ERROR;
  if (d->sourcePos + length > d->sourceLen || d->destPos + length > d->destLen) {
    return TINF_BUF_ERROR;
  }
  std::memcpy(d->dest + d->destPos, d->source + d->sourcePos, length);
  d->sourcePos += length;
  d->destPos += length;
  return TINF_OK;
}

void tinf_build_fixed_trees(TinfTree* lt, TinfTree* dt) {
  unsigned char lengths[288];
  int i = 0;
  for (; i <= 143; ++i) lengths[i] = 8;
  for (; i <= 255; ++i) lengths[i] = 9;
  for (; i <= 279; ++i) lengths[i] = 7;
  for (; i <= 287; ++i) lengths[i] = 8;
  tinf_build_tree(lt, lengths, 288);

  for (i = 0; i < 32; ++i) lengths[i] = 5;
  tinf_build_tree(dt, lengths, 32);
}

int tinf_inflate_block_data(TinfData* d, const TinfTree* lt, const TinfTree* dt) {
  static const unsigned short length_base[29] = {
      3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
  static const unsigned char length_extra[29] = {
      0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
  static const unsigned short dist_base[30] = {
      1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
      193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
  static const unsigned char dist_extra[30] = {
      0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

  while (true) {
    int sym = tinf_decode_symbol(d, lt);
    if (sym < 0 || sym > 285) return TINF_DATA_ERROR;
    if (sym == 256) return TINF_OK;

    if (sym < 256) {
      if (d->destPos >= d->destLen) return TINF_BUF_ERROR;
      d->dest[d->destPos++] = static_cast<unsigned char>(sym);
    } else {
      sym -= 257;
      unsigned int length = tinf_read_bits(d, length_extra[sym], length_base[sym]);
      int dist_sym = tinf_decode_symbol(d, dt);
      if (dist_sym < 0 || dist_sym > 29) return TINF_DATA_ERROR;
      unsigned int dist = tinf_read_bits(d, dist_extra[dist_sym], dist_base[dist_sym]);

      if (dist > d->destPos) return TINF_DATA_ERROR;
      if (d->destPos + length > d->destLen) return TINF_BUF_ERROR;

      for (unsigned int i = 0; i < length; ++i) {
        d->dest[d->destPos] = d->dest[d->destPos - dist];
        d->destPos++;
      }
    }
  }
}

int tinf_inflate_dynamic_block(TinfData* d) {
  static const unsigned char cl_order[19] = {
      16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

  unsigned int hlit = tinf_read_bits(d, 5, 257);
  unsigned int hdist = tinf_read_bits(d, 5, 1);
  unsigned int hclen = tinf_read_bits(d, 4, 4);

  if (hlit > 286 || hdist > 30) return TINF_DATA_ERROR;

  unsigned char cl_lengths[19] = {0};
  for (unsigned int i = 0; i < hclen; ++i) {
    cl_lengths[cl_order[i]] = static_cast<unsigned char>(tinf_read_bits(d, 3, 0));
  }

  TinfTree cl_tree;
  tinf_build_tree(&cl_tree, cl_lengths, 19);

  unsigned char lengths[286 + 30];
  unsigned int num_total = hlit + hdist;
  unsigned int num = 0;

  while (num < num_total) {
    int sym = tinf_decode_symbol(d, &cl_tree);
    if (sym < 0) return TINF_DATA_ERROR;

    if (sym < 16) {
      lengths[num++] = static_cast<unsigned char>(sym);
    } else if (sym == 16) {
      if (num == 0) return TINF_DATA_ERROR;
      unsigned char prev = lengths[num - 1];
      unsigned int count = tinf_read_bits(d, 2, 3);
      if (num + count > num_total) return TINF_DATA_ERROR;
      while (count--) lengths[num++] = prev;
    } else if (sym == 17) {
      unsigned int count = tinf_read_bits(d, 3, 3);
      if (num + count > num_total) return TINF_DATA_ERROR;
      while (count--) lengths[num++] = 0;
    } else if (sym == 18) {
      unsigned int count = tinf_read_bits(d, 7, 11);
      if (num + count > num_total) return TINF_DATA_ERROR;
      while (count--) lengths[num++] = 0;
    }
  }

  TinfTree lt, dt;
  tinf_build_tree(&lt, lengths, hlit);
  tinf_build_tree(&dt, lengths + hlit, hdist);

  return tinf_inflate_block_data(d, &lt, &dt);
}

}  // namespace

extern "C" int tinf_uncompress(void* dest, unsigned int* destLen,
                               const void* source, unsigned int sourceLen) {
  TinfData d;
  d.source = static_cast<const unsigned char*>(source);
  d.sourceLen = sourceLen;
  d.sourcePos = 0;
  d.tag = 0;
  d.bitcount = 0;

  d.dest = static_cast<unsigned char*>(dest);
  d.destLen = *destLen;
  d.destPos = 0;

  int bfinal;
  do {
    bfinal = tinf_getbit(&d);
    int btype = tinf_read_bits(&d, 2, 0);

    int res = TINF_OK;
    switch (btype) {
      case 0:
        res = tinf_inflate_uncompressed_block(&d);
        break;
      case 1: {
        TinfTree lt, dt;
        tinf_build_fixed_trees(&lt, &dt);
        res = tinf_inflate_block_data(&d, &lt, &dt);
        break;
      }
      case 2:
        res = tinf_inflate_dynamic_block(&d);
        break;
      default:
        res = TINF_DATA_ERROR;
        break;
    }
    if (res != TINF_OK) return res;
  } while (!bfinal);

  *destLen = d.destPos;
  return TINF_OK;
}
