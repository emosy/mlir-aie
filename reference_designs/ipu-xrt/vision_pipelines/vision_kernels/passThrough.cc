//===- passThrough.cc -------------------------------------------*- C++ -*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Copyright (C) 2022, Advanced Micro Devices, Inc.
//
//===----------------------------------------------------------------------===//

// #define __AIENGINE__ 1
#define NOCPP

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define REL_WRITE 0
#define REL_READ 1

#include <aie_api/aie.hpp>

template <typename T, int N>
__attribute__((noinline)) void passThrough_aie(T *restrict in, T *restrict out,
                                               const int32_t height,
                                               const int32_t width) {
  //::aie::vector<T, N> data_out;
  //::aie::mask<N> temp_val;
  v64uint8 *restrict outPtr = (v64uint8 *)out;
  v64uint8 *restrict inPtr = (v64uint8 *)in;

  for (int j = 0; j < (height * width); j += N) // Nx samples per loop
    chess_prepare_for_pipelining chess_loop_range(6, ) {
      //::aie::vector<T, N> tmpVector = ::aie::load_v(in);
      //::aie::store_v(out, tmpVector);

      *outPtr++ = *inPtr++;

      // in += N;
      // out += N;
    }
}

extern "C" {

#if BIT_WIDTH == 8

void passThroughLine(uint8_t *in, uint8_t *out, int32_t lineWidth) {
  printf("passThroughLine BIT_WIDTH\n");
  passThrough_aie<uint8_t, 64>(in, out, 1, lineWidth);
}

void passThroughTile(uint8_t *in, uint8_t *out, int32_t tileHeight,
                     int32_t tileWidth) {
  printf("passThroughTile BIT_WIDTH\n");
  passThrough_aie<uint8_t, 64>(in, out, tileHeight, tileWidth);
}

#elif BIT_WIDTH == 16

void passThroughLine(int16_t *in, int16_t *out, int32_t lineWidth) {
  printf("passThroughLine BIT_WIDTH\n");
  passThrough_aie<int16_t, 32>(in, out, 1, lineWidth);
}

void passThroughTile(int16_t *in, int16_t *out, int32_t tileHeight,
                     int32_t tileWidth) {
  printf("passThroughTile BIT_WIDTH\n");
  passThrough_aie<int16_t, 32>(in, out, tileHeight, tileWidth);
}

#else // 32

void passThroughLine(int32_t *in, int32_t *out, int32_t lineWidth) {
  printf("passThroughLine BIT_WIDTH\n");
  passThrough_aie<int32_t, 16>(in, out, 1, lineWidth);
}

void passThroughTile(int32_t *in, int32_t *out, int32_t tileHeight,
                     int32_t tileWidth) {
  printf("passThroughTile BIT_WIDTH\n");
  passThrough_aie<int32_t, 16>(in, out, tileHeight, tileWidth);
}

#endif

} // extern "C"
