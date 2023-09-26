//===- AIETargetXAIEV2.cpp --------------------------------------*- C++ -*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// (c) Copyright 2021 Xilinx Inc.
//
//===----------------------------------------------------------------------===//

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/Location.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Target/LLVMIR/Import.h"
#include "mlir/Tools/mlir-translate/MlirTranslateMain.h"
#include "mlir/Transforms/DialectConversion.h"
#include "mlir/Transforms/Passes.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/TargetSelect.h"

#include "aie/Dialect/AIE/AIENetlistAnalysis.h"
#include "aie/Dialect/AIE/IR/AIEDialect.h"
#include "aie/Dialect/AIEX/IR/AIEXDialect.h"

#include "AIETargets.h"

using namespace mlir;
using namespace xilinx;
using namespace xilinx::AIE;
using namespace xilinx::AIEX;

namespace xilinx {
namespace AIE {

// This string is output at the top of the lowered C code.
const char xaie_c_file_header[] = R"code(
// This file was auto-generated by aiecc.py --aie-generate-xaie.

#ifndef MLIR_AIE_QUIET
#define __mlir_aie_verbose(x) x
#else
#define __mlir_aie_verbose(x)
#endif

// The following is a wrapper for the common "if(call() != 0) return 1" pattern.
// Use this only in functions that return int. If the call this wrapper is used
// on does not succeed, the expanded code will exit out of the function 
// containing this macro with an error code.
#define __mlir_aie_try(x) do { \
  AieRC ret = (x); \
  if(ret != XAIE_OK) { \
    return x; \
  } \
} while(0)

static XAie_DmaDimDesc *__mlir_aie_alloc_dim_desc(size_t ndims) {
  XAie_DmaDimDesc *ret = NULL;
  ret = (XAie_DmaDimDesc *)calloc(sizeof(XAie_DmaDimDesc), ndims);
  if(NULL == ret) {
    __mlir_aie_verbose(fprintf(stderr, "Allocating DmaDimDesc failed.\n"));
  }
  return ret;
}

)code";

/*
static std::string shimDMAInstStr(StringRef col, StringRef index) {
  std::string str;
  llvm::raw_string_ostream rss(str);
  rss << "ShimDMAInst_" << col << "_" << index;
  return str;
}*/
static std::string tileLocStr(StringRef col, StringRef row) {
  std::string str;
  llvm::raw_string_ostream rss(str);
  rss << "XAie_TileLoc(" << col << "," << row << ")";
  return str;
}
static std::string tileLocStr(int col, int row) {
  return tileLocStr(std::to_string(col), std::to_string(row));
}
static std::string tileDMAInstStr(StringRef col, StringRef row,
                                  StringRef bdNum) {
  std::string str;
  llvm::raw_string_ostream rss(str);
  rss << "dma_tile" << col << row << "_bd" << bdNum;
  return str;
}
static std::string tileDMAInstStr(int col, int row, int bdNum) {
  return tileDMAInstStr(std::to_string(col), std::to_string(row),
                        std::to_string(bdNum));
}
static std::string tileDMAInstRefStr(StringRef col, StringRef row,
                                     StringRef bdNum) {
  std::string str;
  llvm::raw_string_ostream rss(str);
  rss << "&(" << tileDMAInstStr(col, row, bdNum) << ")";
  return str;
}
static std::string tileDMAInstRefStr(int col, int row, int bdNum) {
  return tileDMAInstRefStr(std::to_string(col), std::to_string(row),
                           std::to_string(bdNum));
}
static std::string tileDMATensorStr(StringRef col, StringRef row,
                                    StringRef bdNum) {
  std::string str;
  llvm::raw_string_ostream rss(str);
  rss << "dma_tile_" << col << "_" << row << "_bd_" << bdNum << "_tensor";
  return str;
}
static std::string tileDMATensorStr(int col, int row, int bdNum) {
  return tileDMATensorStr(std::to_string(col), std::to_string(row),
                          std::to_string(bdNum));
}
static std::string tileLockStr(StringRef id, StringRef val) {
  std::string str;
  llvm::raw_string_ostream rss(str);
  // rss << "XAie_Lock(" << id << "," << val << ")";
  rss << "XAie_LockInit(" << id << "," << val << ")";
  return str;
}
static std::string packetStr(StringRef id, StringRef type) {
  std::string str;
  llvm::raw_string_ostream rss(str);
  rss << "XAie_PacketInit(" << id << "," << type << ")";
  return str;
}
static std::string packetStr(int id, int type) {
  return packetStr(std::to_string(id), std::to_string(type));
}

// FIXME: code bloat. this shouldn't really be a template, but need
// a proper DMA-like interface
// blockMap: A map that gives a unique bd ID assignment for every block.
template <typename OpType>
mlir::LogicalResult generateDMAConfig(OpType memOp, raw_ostream &output,
                                      const AIETargetModel &target_model,
                                      NetlistAnalysis &NL,
                                      DenseMap<Block *, int> blockMap) {
  StringRef enable = "XAIE_ENABLE";
  StringRef disable = "XAIE_DISABLE";
  StringRef deviceInstRef = "&(ctx->DevInst)"; // TODO

  int col = memOp.colIndex();
  int row = memOp.rowIndex();

  for (auto &block : memOp.getBody()) {
    bool foundBdPacket = false;
    int packetType = 0;
    int packetID = 0;
    bool foundBd = false;
    int lenA = 0;
    int lenB = 0;
    int bytesA = 0;
    int bytesB = 0;
    int offsetA = 0;
    int BaseAddrA = 0;
    bool hasA = false;
    bool hasB = false;
    StringRef bufA = "0";
    StringRef bufB = "0";
    StringRef AbMode = disable;
    int ndims = 0;
    ArrayRef<DimTupleAttr> dims;
    //      StringRef FifoMode = disable; // FIXME: when to enable FIFO mode?
    for (auto op : block.template getOps<DMABDOp>()) {
      foundBd = true;
      ShapedType bufferType =
          op.getBuffer().getType().template cast<::mlir::MemRefType>();
      if (op.isA() && !target_model.isShimNOCTile(col, row)) {
        BaseAddrA = op.getBufferOp().address();
        int bufferCol = op.getBufferOp().getTileOp().colIndex();
        int bufferRow = op.getBufferOp().getTileOp().rowIndex();

        // Memtile DMAs can access neighboring tiles.
        if (target_model.isMemTile(col, row)) {
          if (target_model.isWest(col, row, bufferCol, bufferRow)) {
            BaseAddrA += 0x0;
          } else if (target_model.isInternal(col, row, bufferCol, bufferRow)) {
            BaseAddrA += target_model.getMemTileSize() * 1;
          } else if (target_model.isEast(col, row, bufferCol, bufferRow)) {
            BaseAddrA += target_model.getMemTileSize() * 2;
          }
        }
      }
      if (op.isA() || target_model.isShimNOCTile(col, row)) {
        lenA = op.getLenValue();
        bytesA = bufferType.getElementTypeBitWidth() / 8;
        offsetA = op.getOffsetValue() * bytesA;
        bufA = "XAIEDMA_TILE_BD_ADDRA";
        hasA = true;
      }
      if (op.isB()) {
        lenB = op.getLenValue();
        bytesB = bufferType.getElementTypeBitWidth() / 8;
        bufB = "XAIEDMA_TILE_BD_ADDRB";
        hasB = true;
      }
      if (op.getDimensions()) {
        dims = *op.getDimensions();
        ndims = dims.size();
      }
    }

    if (0 != ndims && AIEArch::AIE2 != target_model.getTargetArch()) {
      return memOp.emitOpError("DMA contains at least one multi-dimensional "
                               "buffer descriptor. This is currently only "
                               "supported for AIE-ML devices.");
    }

    if (hasA && hasB) {
      AbMode = enable;
      if (lenA != lenB)
        llvm::errs() << "ABmode must have matching lengths.\n";
      if (bytesA != bytesB)
        llvm::errs() << "ABmode must have matching element data types.\n";
    }

    int acqValue = 0, relValue = 0;
    bool hasAcq = false, hasRel = false;
    int acqLockID = 0, relLockID = 0;
    for (auto op : block.template getOps<UseLockOp>()) {
      LockOp lock = dyn_cast<LockOp>(op.getLock().getDefiningOp());
      int lockCol = lock.colIndex();
      int lockRow = lock.rowIndex();
      int lockID = lock.getLockIDValue();
      // Memtile DMAs can access neighboring tiles.
      if (target_model.isMemTile(col, row)) {
        if (target_model.isWest(col, row, lockCol, lockRow)) {
          lockID += 0;
        } else if (target_model.isInternal(col, row, lockCol, lockRow)) {
          lockID += target_model.getNumLocks(lockCol, lockRow) * 1;
        } else if (target_model.isEast(col, row, lockCol, lockRow)) {
          lockID += target_model.getNumLocks(lockCol, lockRow) * 2;
        }
      }
      if (op.acquire() || op.acquire_ge()) {
        hasAcq = true;
        acqLockID = lockID;
        acqValue = op.getLockValue();
        if (op.acquire_ge())
          acqValue = -acqValue;
      } else if (op.release()) {
        hasRel = true;
        relLockID = lockID;
        relValue = op.getLockValue();
      } else {
        // unreachable for current targets
        return op.emitOpError("unsupported lock action");
      }
    }

    for (auto op : block.template getOps<DMABDPACKETOp>()) {
      foundBdPacket = true;
      packetType = op.getPacketType();
      packetID = op.getPacketID();
    }

    int bdNum = blockMap[&block];
    if (foundBd) {
      // TODO AB mode separated

      // TODO For now, we are going to name each dma desc with loc and bd
      // which we assume is unique. This is strictly not enforced but in
      // practice, this is true
      output << "XAie_DmaDesc " << tileDMAInstStr(col, row, bdNum) << ";\n";
      output << "__mlir_aie_try(XAie_DmaDescInit(" << deviceInstRef << ", "
             << tileDMAInstRefStr(col, row, bdNum) << ", "
             << tileLocStr(col, row) << "));\n";
      if (hasAcq || hasRel) {
        output << "__mlir_aie_try(XAie_DmaSetLock("
               << tileDMAInstRefStr(col, row, bdNum) << ", "
               << "XAie_LockInit(" << acqLockID << "," << acqValue << "),"
               << "XAie_LockInit(" << relLockID << "," << relValue << ")));\n";
        if (!hasAcq)
          output << tileDMAInstStr(col, row, bdNum)
                 << ".LockDesc.LockAcqEn = " << disable << ";\n";
        if (!hasRel)
          output << tileDMAInstStr(col, row, bdNum)
                 << ".LockDesc.LockRelEn = " << disable << ";\n";
      }

      if (0 == ndims) {
        if (target_model.isShimNOCTile(col, row)) {
          output << "__mlir_aie_try(XAie_DmaSetAddrLen("
                 << tileDMAInstRefStr(col, row, bdNum) << ", /* addrA */ "
                 << "mlir_aie_external_get_addr_myBuffer_" << col << row << "_"
                 << bdNum << "(), "
                 << " /* len */ " << lenA << " * " << bytesA << "));\n";
          output << "__mlir_aie_try(XAie_DmaSetAxi("
                 << tileDMAInstRefStr(col, row, bdNum) << ", "
                 << "/* smid */ 0, "
                 << "/* burstlen */ 4, "
                 << "/* QoS */ 0, "
                 << "/* Cache */ 0, "
                 << "/* Secure */ " << enable << "));\n";
        } else {
          output << "__mlir_aie_try(XAie_DmaSetAddrLen("
                 << tileDMAInstRefStr(col, row, bdNum) << ", /* addrA */ "
                 << "0x" << llvm::utohexstr(BaseAddrA + offsetA) << ", "
                 << " /* len */ " << lenA << " * " << bytesA << "));\n";
        }
      } else {
        std::string tensor = tileDMATensorStr(col, row, bdNum);
        output << "XAie_DmaTensor " << tensor << " = {};\n";
        output << tensor << ".NumDim = " << std::to_string(ndims) << ";\n";
        output << tensor
               << ".Dim ="
                  "__mlir_aie_alloc_dim_desc("
               << std::to_string(ndims) << ");\n";
        output << "if(NULL == " << tensor << ".Dim){\n"
               << "  return 1;\n"
               << "}\n";
        for (int i = 0; i < ndims; i++) {
          // Pass down dimensions in reverse order; in the MLIR, this allows us
          // to specify step sizes/wraps in the same order as we would access a
          // multi-dim C array, with the highest dimension first.
          int j = ndims - i - 1;
          // Assume AIE-ML architecture; we assert this above
          output << tensor << ".Dim[" << std::to_string(j) << "].AieMlDimDesc"
                 << " = { /* StepSize */ "
                 << std::to_string(dims[i].getStepsize()) << ", /* Wrap */ "
                 << std::to_string(dims[i].getWrap()) << "};\n";
        }
        output << "__mlir_aie_try(XAie_DmaSetMultiDimAddr("
               << tileDMAInstRefStr(col, row, bdNum) << ", "
               << "&" << tensor << ", "
               << "0x" << llvm::utohexstr(BaseAddrA + offsetA) << ", "
               << " /* len */ " << lenA << " * " << bytesA << "));\n";
        // TODO: Probably need special handling for NOC
        // TODO: Might need to adjust step sizes / wraps by -1
      }

      if (block.getNumSuccessors() > 0) {
        Block *nextBlock = block.getSuccessors()[0]; // should have only one
                                                     // successor block

        int enableNextBd = 1;
        if (!nextBlock->getOps<EndOp>().empty())
          enableNextBd = 0;

        int nextBdNum = blockMap[nextBlock];
        output << "__mlir_aie_try(XAie_DmaSetNextBd("
               << tileDMAInstRefStr(col, row, bdNum) << ", "
               << " /* nextbd */ " << nextBdNum << ", "
               << " /* enableNextBd */ " << enableNextBd << "));\n";
      }
      if (foundBdPacket) {
        output << "__mlir_aie_try(XAie_DmaSetPkt("
               << tileDMAInstRefStr(col, row, bdNum) << ", "
               << packetStr(packetID, packetType) << "));\n";
      }
      output << "__mlir_aie_try(XAie_DmaEnableBd("
             << tileDMAInstRefStr(col, row, bdNum) << "));\n";
      output << "__mlir_aie_try(XAie_DmaWriteBd(" << deviceInstRef << ", "
             << tileDMAInstRefStr(col, row, bdNum) << ", "
             << tileLocStr(col, row) << ", "
             << " /* bd */ " << bdNum << "));\n";
    }
  }

  for (auto &block : memOp.getBody()) {
    for (auto op : block.template getOps<DMAStartOp>()) {
      int bdNum = blockMap[op.getDest()];

      llvm::StringRef dmaDir = stringifyDMAChannelDir(op.getChannelDir());
      int chNum = op.getChannelIndex();

      output << "__mlir_aie_try(XAie_DmaChannelPushBdToQueue(" << deviceInstRef
             << ", " << tileLocStr(col, row) << ", "
             << "/* ChNum */" << chNum
             << ", "
             // TODO hack until physical dialect changes
             << "/* dmaDir */ DMA_" << dmaDir << ", "
             << "/* BdNum */" << bdNum << "));\n";
      output << "__mlir_aie_try(XAie_DmaChannelEnable(" << deviceInstRef << ", "
             << tileLocStr(col, row) << ", "
             << "/* ChNum */ " << chNum
             << ", "
             // TODO hack until physical dialect changes
             << "/* dmaDir */ DMA_" << dmaDir << "));\n";
    }
  }
  return success();
}

mlir::LogicalResult AIETranslateToXAIEV2(ModuleOp module, raw_ostream &output) {
  //  StringRef ctx   = "ctx";                     // TODO
  StringRef ctx_p = "aie_libxaie_ctx_t* ctx"; // TODO
  //  StringRef deviceInst = "ctx->DevInst";       // TODO
  StringRef deviceInstRef = "&(ctx->DevInst)"; // TODO

  DenseMap<std::pair<int, int>, Operation *> tiles;
  DenseMap<Operation *, CoreOp> cores;
  DenseMap<Operation *, MemOp> mems;
  DenseMap<std::pair<Operation *, int>, LockOp> locks;
  DenseMap<Operation *, SmallVector<BufferOp, 4>> buffers;
  DenseMap<Operation *, SwitchboxOp> switchboxes;

  if (module.getOps<DeviceOp>().empty()) {
    return module.emitOpError("expected AIE.device operation at toplevel");
  }
  DeviceOp targetOp = *(module.getOps<DeviceOp>().begin());
  const auto &target_model = targetOp.getTargetModel();

  NetlistAnalysis NL(targetOp, tiles, cores, mems, locks, buffers, switchboxes);
  NL.collectTiles(tiles);
  NL.collectBuffers(buffers);

  //---------------------------------------------------------------------------
  // mlir_aie_init_libxaie
  //---------------------------------------------------------------------------
  output << xaie_c_file_header;
  output << "aie_libxaie_ctx_t* mlir_aie_init_libxaie() {\n";
  output << "  aie_libxaie_ctx_t *ctx = new aie_libxaie_ctx_t;\n";
  output << "  if (!ctx)\n";
  output << "    return 0;\n";
  auto arch = target_model.getTargetArch();
  std::string AIE1_device("XAIE_DEV_GEN_AIE");
  std::string AIE2_device("XAIE_DEV_GEN_AIEML");
  std::string device;
  int col_shift = 0;
  int row_shift = 0;
  switch (arch) {
  case AIEArch::AIE1:
    device = AIE1_device;
    col_shift = 23;
    row_shift = 18;
    break;
  case AIEArch::AIE2:
    device = AIE2_device;
    col_shift = 25;
    row_shift = 20;
    break;
  }
  assert(col_shift);
  assert(row_shift);
  output << "  ctx->AieConfigPtr.AieGen = " << device << ";\n";
  output << "  ctx->AieConfigPtr.BaseAddr = 0x20000000000;\n";
  output << "  ctx->AieConfigPtr.ColShift = " << col_shift << ";\n";
  output << "  ctx->AieConfigPtr.RowShift = " << row_shift << ";\n";
  output << "  ctx->AieConfigPtr.NumRows = " << target_model.rows() << ";\n";
  output << "  ctx->AieConfigPtr.NumCols = " << target_model.columns() << ";\n";
  output << "  ctx->AieConfigPtr.ShimRowNum = 0;\n";
  output << "  ctx->AieConfigPtr.MemTileRowStart = 1;\n";
  output << "  ctx->AieConfigPtr.MemTileNumRows = "
         << target_model.getNumMemTileRows() << ";\n";
  output << "  //  ctx->AieConfigPtr.ReservedRowStart = "
            "XAIE_RES_TILE_ROW_START;\n";
  output
      << "  //  ctx->AieConfigPtr.ReservedNumRows  = XAIE_RES_TILE_NUM_ROWS;\n";
  output << "  ctx->AieConfigPtr.AieTileRowStart = "
         << (1 + target_model.getNumMemTileRows()) << ";\n";
  output << "  ctx->AieConfigPtr.AieTileNumRows = "
         << (target_model.rows() - 1 - target_model.getNumMemTileRows())
         << ";\n";
  output << "  ctx->AieConfigPtr.PartProp = {0};\n";
  output << "  ctx->DevInst = {0};\n";
  output << "  return ctx;\n";
  output << "}\n";
  output << "\n";

  //---------------------------------------------------------------------------
  // mlir_aie_configure_cores
  //---------------------------------------------------------------------------
  output << "int mlir_aie_configure_cores(" << ctx_p << ") {\n";
  // Reset each core.  Load the corresponding ELF file, if necessary.
  for (auto tileOp : targetOp.getOps<TileOp>()) {
    int col = tileOp.colIndex();
    int row = tileOp.rowIndex();
    if (tileOp.isShimTile() || tileOp.isMemTile()) {
      // Resets no needed with V2 kernel driver
    } else {
      // Resets no needed with V2 kernel driver
      output << "__mlir_aie_try(XAie_CoreReset(" << deviceInstRef << ", "
             << tileLocStr(col, row) << "));\n";
      output << "__mlir_aie_try(XAie_CoreDisable(" << deviceInstRef << ", "
             << tileLocStr(col, row) << "));\n";
      // Release locks
      int numLocks = target_model.getNumLocks(col, row);
      output << "for (int l = 0; l < " << numLocks << "; ++l)\n"
             << "  __mlir_aie_try(XAie_LockRelease(" << deviceInstRef << ", "
             << tileLocStr(col, row) << ", XAie_LockInit(l, 0x0), 0));\n";
      if (auto coreOp = tileOp.getCoreOp()) {
        std::string fileName;
        if (auto fileAttr = coreOp->getAttrOfType<StringAttr>("elf_file")) {
          fileName = std::string(fileAttr.getValue());
        } else {
          fileName = std::string("core_") + std::to_string(col) + "_" +
                     std::to_string(row) + ".elf";
        }
        output << "{\n"
               << "AieRC RC = XAie_LoadElf(" << deviceInstRef << ", "
               << tileLocStr(col, row) << ", "
               << "(const char*)\"" << fileName << "\",0);\n";
        output << "if (RC != XAIE_OK)\n"
               << "    __mlir_aie_verbose(fprintf(stderr, \"Failed to load elf "
                  "for Core[%d,%d], ret is %d\\n\", "
               << std::to_string(col) << ", " << std::to_string(row)
               << ", RC));\n"
               << "assert(RC == XAIE_OK);\n"
               << "}\n";
      }
    }
  }
  output << "return XAIE_OK;\n";
  output << "} // mlir_aie_configure_cores\n\n";

  //---------------------------------------------------------------------------
  // mlir_aie_start_cores
  //---------------------------------------------------------------------------
  output << "int mlir_aie_start_cores(" << ctx_p << ") {\n";
  // Start execution of all the cores.
  for (auto tileOp : targetOp.getOps<TileOp>()) {
    int col = tileOp.colIndex();
    int row = tileOp.rowIndex();
    if (!tileOp.isShimTile() && !tileOp.isMemTile()) {
      output << "__mlir_aie_try(XAie_CoreUnreset(" << deviceInstRef << ", "
             << tileLocStr(col, row) << "));\n";
      output << "__mlir_aie_try(XAie_CoreEnable(" << deviceInstRef << ", "
             << tileLocStr(col, row) << "));\n";
    }
  }
  output << "return XAIE_OK;\n";
  output << "} // mlir_aie_start_cores\n\n";

  //---------------------------------------------------------------------------
  // mlir_aie_configure_dmas
  //---------------------------------------------------------------------------
  output << "int mlir_aie_configure_dmas(" << ctx_p << ") {\n";

  // DMA configuration
  // AieRC XAie_DmaDescInit(XAie_DevInst *DevInst, XAie_DmaDesc *DmaDesc,
  // XAie_LocType Loc); AieRC XAie_DmaSetLock(XAie_DmaDesc *DmaDesc, XAie_Lock
  // Acq, XAie_Lock Rel); AieRC XAie_DmaSetPkt(XAie_DmaDesc *DmaDesc,
  // XAie_Packet Pkt); AieRC XAie_DmaSetOutofOrderBdId(XAie_DmaDesc *DmaDesc, u8
  // OutofOrderBdId); AieRC XAie_DmaSetDoubleBuffer(XAie_DmaDesc *DmaDesc, u64
  // Addr, XAie_Lock Acq, XAie_Lock Rel); AieRC XAie_DmaSetAddrLen(XAie_DmaDesc
  // *DmaDesc, u64 Addr, u32 Len); AieRC XAie_DmaSetMultiDimAddr(XAie_DmaDesc
  // *DmaDesc, XAie_DmaTensor *Tensor, u64 Addr, u32 Len); AieRC
  // XAie_DmaEnableCompression(XAie_DmaDesc *DmaDesc); AieRC
  // XAie_DmaSetNextBd(XAie_DmaDesc *DmaDesc, u8 NextBd, u8 EnableNextBd); AieRC
  // XAie_DmaEnableBd(XAie_DmaDesc *DmaDesc); AieRC
  // XAie_DmaDisableBd(XAie_DmaDesc *DmaDesc); AieRC XAie_DmaSetAxi(XAie_DmaDesc
  // *DmaDesc, u8 Smid, u8 BurstLen, u8 Qos,u8 Cache, u8 Secure); AieRC
  // XAie_DmaSetInterleaveEnable(XAie_DmaDesc *DmaDesc, u8 DoubleBuff, u8
  // IntrleaveCount, u16 IntrleaveCurr); AieRC XAie_DmaWriteBd(XAie_DevInst
  // *DevInst, XAie_DmaDesc *DmaDesc, XAie_LocType Loc, u8 BdNum);

  // AieRC XAie_DmaChannelResetAll(XAie_DevInst *DevInst, XAie_LocType Loc,
  // XAie_DmaChReset Reset); AieRC XAie_DmaChannelReset(XAie_DevInst *DevInst,
  // XAie_LocType Loc, u8 ChNum, XAie_DmaDirection Dir, XAie_DmaChReset Reset);
  // AieRC XAie_DmaChannelPauseStream(XAie_DevInst *DevInst, XAie_LocType Loc,
  // u8 ChNum, XAie_DmaDirection Dir, u8 Pause); AieRC
  // XAie_DmaChannelPauseMem(XAie_DevInst *DevInst, XAie_LocType Loc, u8 ChNum
  // XAie_DmaDirection Dir, u8 Pause); AieRC XAie_DmaChannelConfig(XAie_DevInst
  // *DevInst, XAie_DmaDesc *DmaDesc, XAie_LocType Loc, u8 ChNum,
  // XAie_DmaDirection Dir, u8 RepeatCount, u8 EnTokenIssue, u8 ControllerId);
  // AieRC XAie_DmaChannelPushBdToQueue(XAie_DevInst *DevInst, XAie_LocType Loc,
  // u8 ChNum, XAie_DmaDirection Dir, u8 BdNum); AieRC
  // XAie_DmaChannelEnable(XAie_DevInst *DevInst, XAie_LocType Loc, u8 ChNum,
  // XAie_DmaDirection Dir); AieRC XAie_DmaChannelDisable(XAie_DevInst *DevInst,
  // XAie_LocType Loc, u8 ChNum, XAie_DmaDirection Dir);
  for (auto memOp : targetOp.getOps<MemOp>()) {
    DenseMap<Block *, int> blockMap;

    // Assign each block a BD number
    int bdNum = 0;
    for (auto &block : memOp.getBody()) {
      if (!block.getOps<DMABDOp>().empty()) {
        blockMap[&block] = bdNum;
        bdNum++;
      }
    }
    auto result = generateDMAConfig(memOp, output, target_model, NL, blockMap);
    if (result.failed())
      return result;
  }
  for (auto memOp : targetOp.getOps<MemTileDMAOp>()) {
    DenseMap<Block *, int> blockMap;
    // Memtiles have restrictions on which channels can access which BDs
    DenseMap<Block *, int> channelMap;

    for (auto &block : memOp.getBody()) {
      for (auto op : block.getOps<DMAStartOp>()) {
        int chNum = op.getChannelIndex();
        channelMap[&block] = chNum;
        auto dest = op.getDest();
        while (dest) {
          channelMap[dest] = chNum;
          if (dest->getSuccessors().size() < 1)
            break;
          dest = dest->getSuccessors()[0];
          if (channelMap.count(dest))
            break;
        }
      }
    }

    // Assign each block a BD number
    int evenBdNum = 0;
    int oddBdNum = 24;
    for (auto &block : memOp.getBody()) {
      if (block.getOps<DMABDOp>().empty())
        continue;
      assert(channelMap.count(&block));
      if (channelMap[&block] & 1)
        blockMap[&block] = oddBdNum++;
      else
        blockMap[&block] = evenBdNum++;
    }
    auto result = generateDMAConfig(memOp, output, target_model, NL, blockMap);
    if (result.failed())
      return result;
  }

  output << "return XAIE_OK;\n";
  output << "} // mlir_aie_configure_dmas\n\n";

  for (auto op : targetOp.getOps<ExternalBufferOp>()) {
    if (op.hasName()) {
      output << "static u64 _mlir_aie_external_" << op.name().getValue()
             << ";\n";
      output << "static bool _mlir_aie_external_set_" << op.name().getValue()
             << " = false;\n";

      output << "void mlir_aie_external_set_addr_" << op.name().getValue()
             << "(" << ctx_p << ", u64 VA) {\n"
             << "  u64 device_address = mlir_aie_get_device_address(ctx, (void "
                "*)VA);\n"
             << "    _mlir_aie_external_set_" << op.name().getValue()
             << " = true;\n"
             << "    _mlir_aie_external_" << op.name().getValue()
             << " = device_address;\n"
             << "}\n";
    }
  }

  // ShimDMA Config
  //  int index = 0;
  for (auto op : targetOp.getOps<ShimDMAOp>()) {
    int col = op.colIndex();
    int row = op.rowIndex();

    DenseMap<Block *, int> blockMap;
    {
      // Assign each block a BD number
      int bdNum = 0;
      for (auto &block : op.getBody()) {
        if (!block.getOps<DMABDOp>().empty()) {
          blockMap[&block] = bdNum;

          uint64_t offset = 0;
          for (auto op : block.getOps<DMABDOp>()) {
            offset = op.getOffsetValue();
            auto buffer = cast<xilinx::AIE::ExternalBufferOp>(
                op.getBuffer().getDefiningOp());

            output << "u64 mlir_aie_external_get_addr_myBuffer_" << col << row
                   << "_" << bdNum << "(void) {\n"
                   << "    assert(_mlir_aie_external_set_"
                   << buffer.name().getValue() << ");\n"
                   << "    return _mlir_aie_external_"
                   << buffer.name().getValue() << " + "
                   << llvm::utohexstr(offset) << ";\n"
                   << "}\n";
          }

          bdNum++;
        }
      }
    }

    output << "int mlir_aie_configure_shimdma_" << col << row << "(" << ctx_p
           << ") {\n";
    auto result = generateDMAConfig(op, output, target_model, NL, blockMap);
    if (result.failed())
      return result;
    output << "return XAIE_OK;\n";
    output << "} // mlir_aie_configure_shimdma\n\n";
  }

  //---------------------------------------------------------------------------
  // mlir_aie_initialize_locks
  //---------------------------------------------------------------------------
  output << "int mlir_aie_initialize_locks(" << ctx_p << ") {\n";
  // Lock configuration
  for (auto lock : targetOp.getOps<LockOp>()) {
    TileOp tile = lock.getTileOp();
    int col = tile.colIndex();
    int row = tile.rowIndex();
    int lockID = lock.getLockIDValue();
    auto init = lock.getInit();
    if (init) {
      output << "__mlir_aie_try(XAie_LockSetValue(" << deviceInstRef << ", "
             << tileLocStr(col, row) << ", "
             << "XAie_LockInit(" << lockID << ", " << *init << ")));\n";
    }
  }
  output << "return XAIE_OK;\n";
  output << "} // mlir_aie_initialize_locks\n";

  //---------------------------------------------------------------------------
  // mlir_aie_configure_switchboxes
  //---------------------------------------------------------------------------
  output << "int mlir_aie_configure_switchboxes(" << ctx_p << ") {\n";
  output << "  int x, y;\n";

  // StreamSwitch (switchbox) configuration
  for (auto switchboxOp : targetOp.getOps<SwitchboxOp>()) {
    Region &r = switchboxOp.getConnections();
    Block &b = r.front();
    bool isEmpty = b.getOps<ConnectOp>().empty() &&
                   b.getOps<MasterSetOp>().empty() &&
                   b.getOps<PacketRulesOp>().empty();
    bool isParam = false;

    if (isa<TileOp>(switchboxOp.getTile().getDefiningOp())) {
      int col = switchboxOp.colIndex();
      int row = switchboxOp.rowIndex();
      if (!isEmpty) {
        output << "// Core Stream Switch column " << col << " row " << row
               << "\n";
        output << "x = " << col << ";\n";
        output << "y = " << row << ";\n";
      }
    } else if (AIEX::SelectOp sel = dyn_cast<AIEX::SelectOp>(
                   switchboxOp.getTile().getDefiningOp())) {
      // parameterize streamswitch's configuration
      isParam = true;
      HerdOp sourceHerd = dyn_cast<HerdOp>(sel.getStartHerd().getDefiningOp());
      std::string sourceHerdName(sourceHerd.name().getValue());

      IterOp iterX = dyn_cast<IterOp>(sel.getIterX().getDefiningOp());
      IterOp iterY = dyn_cast<IterOp>(sel.getIterY().getDefiningOp());
      int startXValue = iterX.getStartValue();
      int endXValue = iterX.getEndValue();
      int strideXValue = iterX.getStrideValue();
      int startYValue = iterY.getStartValue();
      int endYValue = iterY.getEndValue();
      int strideYValue = iterY.getStrideValue();

      std::string startX(sourceHerdName + "_X + " +
                         std::to_string(startXValue));
      std::string endX(sourceHerdName + "_X + " + std::to_string(endXValue));
      std::string startY(sourceHerdName + "_Y + " +
                         std::to_string(startYValue));
      std::string endY(sourceHerdName + "_Y + " + std::to_string(endYValue));

      output << "for (x = " << startX << "; x < " << endX
             << "; x += " << strideXValue << ") {\n";
      output << "for (y = " << startY << "; y < " << endY
             << "; y += " << strideYValue << ") {\n";
    }

    for (auto connectOp : b.getOps<ConnectOp>()) {
      output << "__mlir_aie_try(XAie_StrmConnCctEnable(" << deviceInstRef
             << ", " << tileLocStr("x", "y") << ", "
             << stringifyWireBundle(connectOp.getSourceBundle()).upper() << ", "
             << connectOp.sourceIndex() << ", "
             << stringifyWireBundle(connectOp.getDestBundle()).upper() << ", "
             << connectOp.destIndex() << "));\n";
    }

    for (auto connectOp : b.getOps<MasterSetOp>()) {
      int mask = 0;
      int arbiter = -1;
      for (auto val : connectOp.getAmsels()) {
        AMSelOp amsel = dyn_cast<AMSelOp>(val.getDefiningOp());
        arbiter = amsel.arbiterIndex();
        int msel = amsel.getMselValue();
        mask |= (1 << msel);
      }
      bool isdma = (connectOp.getDestBundle() == WireBundle::DMA);

      output << "__mlir_aie_try(XAie_StrmPktSwMstrPortEnable(" << deviceInstRef
             << ", " << tileLocStr("x", "y") << ", "
             << stringifyWireBundle(connectOp.getDestBundle()).upper() << ", "
             << connectOp.destIndex() << ", "
             << "/* drop_header */ "
             << (isdma ? "XAIE_SS_PKT_DROP_HEADER"
                       : "XAIE_SS_PKT_DONOT_DROP_HEADER")
             << ", "
             << "/* arbiter */ " << arbiter << ", "
             << "/* MSelEn */ "
             << "0x" << llvm::utohexstr(mask) << "));\n";
    }

    for (auto connectOp : b.getOps<PacketRulesOp>()) {
      int slot = 0;
      Block &block = connectOp.getRules().front();
      for (auto slotOp : block.getOps<PacketRuleOp>()) {
        AMSelOp amselOp = dyn_cast<AMSelOp>(slotOp.getAmsel().getDefiningOp());
        int arbiter = amselOp.arbiterIndex();
        int msel = amselOp.getMselValue();
        output << "__mlir_aie_try(XAie_StrmPktSwSlavePortEnable("
               << deviceInstRef << ", " << tileLocStr("x", "y") << ", "
               << stringifyWireBundle(connectOp.getSourceBundle()).upper()
               << ", " << connectOp.sourceIndex() << "));\n";

        // TODO Need to better define packet id,type used here
        output << "__mlir_aie_try(XAie_StrmPktSwSlaveSlotEnable("
               << deviceInstRef << ", " << tileLocStr("x", "y") << ", "
               << stringifyWireBundle(connectOp.getSourceBundle()).upper()
               << ", " << connectOp.sourceIndex() << ", "
               << "/* slot */ " << slot << ", "
               << "/* packet */ " << packetStr(slotOp.valueInt(), /*type*/ 0)
               << ", "
               << "/* mask */ "
               << "0x" << llvm::utohexstr(slotOp.maskInt()) << ", "
               << "/* msel */ " << msel << ", "
               << "/* arbiter */ " << arbiter << "));\n";
        slot++;
      }
    }

    if (isParam) {
      output << "}\n";
      output << "}\n";
    }
  }
  for (auto op : targetOp.getOps<ShimMuxOp>()) {
    Region &r = op.getConnections();
    Block &b = r.front();
    bool isEmpty = b.getOps<ConnectOp>().empty();

    if (isa<TileOp>(op.getTile().getDefiningOp())) {
      int col = op.colIndex();
      int row = op.rowIndex();
      if (!isEmpty) {
        output << "// ShimMux column " << col << " row " << row << "\n";
        output << "// NOTE ShimMux always connects from the south as "
               << "directions are defined relative to the tile stream "
               << "switch\n";
        output << "x = " << col << ";\n";
        output << "y = " << row << ";\n";
      }
    }

    for (auto connectOp : b.getOps<ConnectOp>()) {
      if (connectOp.getSourceBundle() == WireBundle::North) {
        // demux!
        output
            << "__mlir_aie_try(XAie_EnableAieToShimDmaStrmPort("
            << deviceInstRef << ", " << tileLocStr("x", "y")
            << ", "
            //               <<
            //               stringifyWireBundle(connectOp.sourceBundle()).upper()
            << connectOp.sourceIndex() << "));\n";
      } else if (connectOp.getDestBundle() == WireBundle::North) {
        // mux
        output
            << "__mlir_aie_try(XAie_EnableShimDmaToAieStrmPort("
            << deviceInstRef << ", " << tileLocStr("x", "y")
            << ", "
            //               <<
            //               stringifyWireBundle(connectOp.sourceBundle()).upper()
            << connectOp.destIndex() << "));\n";
      }
    }
  }
  for (auto switchboxOp : targetOp.getOps<ShimSwitchboxOp>()) {
    Region &r = switchboxOp.getConnections();
    Block &b = r.front();
    bool isEmpty = b.getOps<ConnectOp>().empty();
    int col = switchboxOp.getCol();
    if (!isEmpty) {
      output << "// Shim Switch column " << col << "\n";
    }
    for (auto connectOp : b.getOps<ConnectOp>()) {
      output << "__mlir_aie_try(XAie_StrmConnCctEnable(" << deviceInstRef
             << ", " << tileLocStr(col, 0) << ", "
             << stringifyWireBundle(connectOp.getSourceBundle()).upper() << ", "
             << connectOp.sourceIndex() << ", "
             << stringifyWireBundle(connectOp.getDestBundle()).upper() << ", "
             << connectOp.destIndex() << "));\n";
    }
  }

  output << "return XAIE_OK;\n";
  output << "} // mlir_aie_configure_switchboxes\n\n";

  //---------------------------------------------------------------------------
  // Output Buffer Accessors
  //---------------------------------------------------------------------------
  for (auto tile : tiles) {
    Operation *tileOp = tile.second;
    std::pair<int, int> coord = NL.getCoord(tileOp);
    int col = coord.first;
    int row = coord.second;
    auto loc = tileLocStr(col, row);

    auto bufferAccessor = [&](Optional<TileID> tile, BufferOp buf) {
      // int32_t mlir_aie_read_buffer_a13(int index) {
      // void mlir_aie_write_buffer_a13(int index, int32_t value) {
      std::string bufName(buf.name().getValue());
      Type t = buf.getType();
      Type et;
      std::string typestr;
      if (auto memrefType = t.dyn_cast<MemRefType>()) {
        et = memrefType.getElementType();
        if (et.isInteger(32))
          typestr = "int32_t";
        else if (et.isF32())
          typestr = "float";
        else {
          output << "// buffer " << bufName << " with unsupported type " << t
                 << ";\n";
          return; // Unsupported type
        }

      } else {
        output << "// buffer " << bufName << " with unsupported type " << t
               << ";\n";
        return; // Unsupported type
      }

      output << "const int " << bufName << "_offset = " << buf.address()
             << ";\n";
      output << typestr << " mlir_aie_read_buffer_" << bufName << "(" << ctx_p
             << ", int index) {\n";
      output << "u32 value; auto rc = XAie_DataMemRdWord(" << deviceInstRef
             << ", " << loc << ", " << bufName
             << "_offset + (index*4), &value);\n";
      if (et.isInteger(32))
        output << "  return value;\n";
      else if (et.isF32()) {
        output << "  union caster { int32_t i; float f; };\n";
        output << "  caster c; c.i = value;\n";
        output << "  return c.f;\n";
      }
      output << "}\n";
      output << "int mlir_aie_write_buffer_" << bufName << "(" << ctx_p
             << ", int index, " << typestr << " value) {\n";
      if (et.isInteger(32))
        output << "  int32_t int_value = value;\n";
      else if (et.isF32()) {
        output << "  union caster { int32_t i; float f; };\n";
        output << "  caster c; c.f = value;\n";
        output << "  int32_t int_value = c.i;\n";
      }
      output << "AieRC rc =    XAie_DataMemWrWord(" << deviceInstRef << ", "
             << loc << ", " << bufName << "_offset + (index*4), int_value);\n";
      output << "return rc;\n";
      output << "}\n";
    };

    // if(tiles.count(tile.getValue()))
    for (auto buf : buffers[tileOp])
      bufferAccessor(coord, buf);
  }

  auto lockAccessor = [&](LockOp lock) {
    int col = lock.colIndex();
    int row = lock.rowIndex();
    if (!lock.hasName())
      return;
    std::string lockName(lock.name().getValue());
    output << "int mlir_aie_acquire_" << lockName << "(" << ctx_p
           << ", int value, int timeout) {\n";
    output << "  const int id = " << lock.getLockIDValue() << ";\n";
    output << "  return XAie_LockAcquire(" << deviceInstRef << ", "
           << tileLocStr(col, row) << ", " << tileLockStr("id", "value")
           << ", timeout);\n";
    output << "}\n";
    output << "int mlir_aie_release_" << lockName << "(" << ctx_p
           << ", int value, int timeout) {\n";
    output << "  const int id = " << lock.getLockIDValue() << ";\n";
    output << "  return XAie_LockRelease(" << deviceInstRef << ", "
           << tileLocStr(col, row) << ", " << tileLockStr("id", "value")
           << ", timeout);\n";
    output << "}\n";
  };

  for (auto lock : targetOp.getOps<LockOp>())
    lockAccessor(lock);

  return success();
}
} // namespace AIE
} // namespace xilinx
