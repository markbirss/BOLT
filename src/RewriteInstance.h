//===--- RewriteInstance.h - Interface for machine-level function ---------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Interface to control an instance of a binary rewriting process.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_BOLT_REWRITE_INSTANCE_H
#define LLVM_TOOLS_LLVM_BOLT_REWRITE_INSTANCE_H

#include "BinaryFunction.h"
#include "DebugData.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h"
#include "llvm/ExecutionEngine/SectionMemoryManager.h"
#include "llvm/MC/StringTableBuilder.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/StringPool.h"
#include <map>
#include <set>

namespace llvm {

class DWARFContext;
class DWARFFrame;
class ToolOutputFile;

namespace bolt {

class BinaryContext;
class CFIReaderWriter;
class DataAggregator;
class DataReader;
class RewriteInstanceDiff;

struct SegmentInfo {
  uint64_t Address;           /// Address of the segment in memory.
  uint64_t Size;              /// Size of the segment in memory.
  uint64_t FileOffset;        /// Offset in the file.
  uint64_t FileSize;          /// Size in file.

  void print(raw_ostream &OS) const {
    OS << "SegmentInfo { Address: 0x"
       << Twine::utohexstr(Address) << ", Size: 0x"
       << Twine::utohexstr(Size) << ", FileOffset: 0x"
       << Twine::utohexstr(FileOffset) << ", FileSize: 0x"
       << Twine::utohexstr(FileSize) << "}";
  };
};

inline raw_ostream &operator<<(raw_ostream &OS, const SegmentInfo &SegInfo) {
  SegInfo.print(OS);
  return OS;
}

/// Class responsible for allocating and managing code and data sections.
class ExecutableFileMemoryManager : public SectionMemoryManager {
private:
  uint8_t *allocateSection(intptr_t Size,
                           unsigned Alignment,
                           unsigned SectionID,
                           StringRef SectionName,
                           bool IsCode,
                           bool IsReadOnly);
  BinaryContext &BC;
  bool AllowStubs;

public:
  /// [start memory address] -> [segment info] mapping.
  std::map<uint64_t, SegmentInfo> SegmentMapInfo;

  ExecutableFileMemoryManager(BinaryContext &BC, bool AllowStubs)
    : BC(BC), AllowStubs(AllowStubs) {}

  ~ExecutableFileMemoryManager();

  uint8_t *allocateCodeSection(uintptr_t Size, unsigned Alignment,
                               unsigned SectionID,
                               StringRef SectionName) override {
    return allocateSection(Size, Alignment, SectionID, SectionName,
                           /*IsCode=*/true, true);
  }

  uint8_t *allocateDataSection(uintptr_t Size, unsigned Alignment,
                               unsigned SectionID, StringRef SectionName,
                               bool IsReadOnly) override {
    return allocateSection(Size, Alignment, SectionID, SectionName,
                           /*IsCode=*/false, IsReadOnly);
  }

  uint8_t *recordNoteSection(const uint8_t *Data, uintptr_t Size,
                             unsigned Alignment, unsigned SectionID,
                             StringRef SectionName) override;

  bool allowStubAllocation() const override { return AllowStubs; }

  bool finalizeMemory(std::string *ErrMsg = nullptr) override;
};

/// This class encapsulates all data necessary to carry on binary reading,
/// disassembly, CFG building, BB reordering (among other binary-level
/// optimizations) and rewriting. It also has the logic to coordinate such
/// events.
class RewriteInstance {
public:
  RewriteInstance(llvm::object::ELFObjectFileBase *File, DataReader &DR,
                  DataAggregator &DA, const int Argc, const char *const *Argv);
  ~RewriteInstance();

  /// Reset all state except for split hints. Used to run a second pass with
  /// function splitting information.
  void reset();

  /// Run all the necessary steps to read, optimize and rewrite the binary.
  void run();

  /// Diff this instance against another one. Non-const since we may run passes
  /// to fold identical functions.
  void compare(RewriteInstance &RI2);

  /// Populate array of binary functions and other objects of interest
  /// from meta data in the file.
  void discoverFileObjects();

  /// Read info from special sections. E.g. eh_frame and .gcc_except_table
  /// for exception and stack unwinding information.
  void readSpecialSections();

  /// Adjust supplied command-line options based on input data.
  void adjustCommandLineOptions();

  /// Read relocations from a given section.
  void readRelocations(const object::SectionRef &Section);

  /// Read information from debug sections.
  void readDebugInfo();

  /// Associate profile data with binary objects.
  void processProfileData();

  /// Disassemble each function in the binary and associate it with a
  /// BinaryFunction object, preparing all information necessary for binary
  /// optimization.
  void disassembleFunctions();

  void postProcessFunctions();

  /// Run optimizations that operate at the binary, or post-linker, level.
  void runOptimizationPasses();

  /// Write all functions to an intermediary object file, map virtual to real
  /// addresses and link this object file, resolving all relocations and
  /// performing final relaxation.
  void emitFunctions();

  /// Emit data \p Section, possibly with relocations. Use name \p Name if
  /// non-empty.
  void emitDataSection(MCStreamer *Streamer,
                       const BinarySection &Section,
                       StringRef Name = StringRef());

  /// Emit data sections that have code references in them.
  void emitDataSections(MCStreamer *Streamer);

  /// Update debug information in the file for re-written code.
  void updateDebugInfo();

  /// Recursively update debug info for all DIEs in \p Unit.
  /// If \p Function is not empty, it points to a function corresponding
  /// to a parent DW_TAG_subprogram node of the current \p DIE.
  void updateUnitDebugInfo(const DWARFDie DIE,
                           std::vector<const BinaryFunction *> FunctionStack);

  /// Map all sections to their final addresses.
  void mapTextSections(orc::VModuleKey ObjectsHandle);
  void mapDataSections(orc::VModuleKey ObjectsHandle);
  void mapFileSections(orc::VModuleKey ObjectsHandle);

  /// Update output object's values based on the final \p Layout.
  void updateOutputValues(const MCAsmLayout &Layout);

  /// Check which functions became larger than their original version and
  /// annotate function splitting information.
  ///
  /// Returns true if any function was annotated, requiring us to perform a
  /// second pass to emit those functions in two parts.
  bool checkLargeFunctions();

  /// Updates debug line information for non-simple functions, which are not
  /// rewritten.
  void updateDebugLineInfoForNonSimpleFunctions();

  /// Rewrite back all functions (hopefully optimized) that fit in the original
  /// memory footprint for that function. If the function is now larger and does
  /// not fit in the binary, reject it and preserve the original version of the
  /// function. If we couldn't understand the function for some reason in
  /// disassembleFunctions(), also preserve the original version.
  void rewriteFile();

  /// Return address of a function in the new binary corresponding to
  /// \p OldAddress address in the original binary.
  uint64_t getNewFunctionAddress(uint64_t OldAddress);

  /// Return value for the symbol \p Name in the output.
  uint64_t getNewValueForSymbol(const StringRef Name) {
    return cantFail(OLT->findSymbol(Name, false).getAddress(),
                    "findSymbol failed");
  }

  /// Return BinaryFunction containing a given \p Address or nullptr if
  /// no registered function has it.
  ///
  /// In a binary a function has somewhat vague  boundaries. E.g. a function can
  /// refer to the first byte past the end of the function, and it will still be
  /// referring to this function, not the function following it in the address
  /// space. Thus we have the following flags that allow to lookup for
  /// a function where a caller has more context for the search.
  ///
  /// If \p CheckPastEnd is true and the \p Address falls on a byte
  /// immediately following the last byte of some function and there's no other
  /// function that starts there, then return the function as the one containing
  /// the \p Address. This is useful when we need to locate functions for
  /// references pointing immediately past a function body.
  ///
  /// If \p UseMaxSize is true, then include the space between this function
  /// body and the next object in address ranges that we check.
  BinaryFunction *getBinaryFunctionContainingAddress(uint64_t Address,
                                                     bool CheckPastEnd = false,
                                                     bool UseMaxSize = false);

  const BinaryFunction *getBinaryFunctionAtAddress(uint64_t Address) const;

  /// Produce output address ranges based on input ranges for some module.
  DWARFAddressRangesVector translateModuleAddressRanges(
      const DWARFAddressRangesVector &InputRanges) const;

private:
  /// Emit a single function.
  void emitFunction(MCStreamer &Streamer, BinaryFunction &Function,
                    bool EmitColdPart);

  /// Detect addresses and offsets available in the binary for allocating
  /// new sections.
  void discoverStorage();

  /// Adjust function sizes and set proper maximum size values after the whole
  /// symbol table has been processed.
  void adjustFunctionBoundaries();

  /// Make .eh_frame section relocatable.
  void relocateEHFrameSection();

  /// Analyze relocation \p Rel contained in section \p RelocatedSection.
  /// Return true if the relocation was successfully processed, false otherwise.
  /// The \p SymbolName, \p SymbolAddress, \p Addend and \p ExtractedValue
  /// parameters will be set on success.
  bool analyzeRelocation(const RelocationRef &Rel,
                         SectionRef RelocatedSection,
                         std::string &SymbolName,
                         bool &IsSectionRelocation,
                         uint64_t &SymbolAddress,
                         int64_t &Addend,
                         uint64_t &ExtractedValue) const;

  /// Rewrite non-allocatable sections with modifications.
  void rewriteNoteSections();

  /// Write .eh_frame_hdr.
  void writeEHFrameHeader();

  /// Disassemble and create function entries for PLT.
  void disassemblePLT();

  /// ELF-specific part. TODO: refactor into new class.
#define ELF_FUNCTION(FUNC)                                                     \
  template <typename ELFT> void FUNC(ELFObjectFile<ELFT> *Obj);                \
  void FUNC() {                                                                \
    if (auto *ELF32LE = dyn_cast<ELF32LEObjectFile>(InputFile))                \
      return FUNC(ELF32LE);                                                    \
    if (auto *ELF64LE = dyn_cast<ELF64LEObjectFile>(InputFile))                \
      return FUNC(ELF64LE);                                                    \
    if (auto *ELF32BE = dyn_cast<ELF32BEObjectFile>(InputFile))                \
      return FUNC(ELF32BE);                                                    \
    auto *ELF64BE = cast<ELF64BEObjectFile>(InputFile);                        \
    return FUNC(ELF64BE);                                                      \
  }

  /// Patch ELF book-keeping info.
  void patchELF();
  void patchELFPHDRTable();

  /// Create section header table.
  ELF_FUNCTION(patchELFSectionHeaderTable);

  /// Create the regular symbol table and patch dyn symbol tables.
  ELF_FUNCTION(patchELFSymTabs);

  /// Patch dynamic section/segment of ELF.
  ELF_FUNCTION(patchELFDynamic);

  /// Patch .got
  ELF_FUNCTION(patchELFGOT);

  /// Patch allocatable relocation sections.
  ELF_FUNCTION(patchELFAllocatableRelaSections);

  /// Finalize memory image of section header string table.
  ELF_FUNCTION(finalizeSectionStringTable);

  /// Get a list of all the sections to include in the output binary along
  /// with a map of input to output indices.  Optionally produce a mapping
  /// of section name to new section index in /p OutputSectionNameMap.
  template <typename ELFT,
            typename ELFShdrTy = typename ELFObjectFile<ELFT>::Elf_Shdr>
  std::vector<uint32_t> getOutputSections(
     ELFObjectFile<ELFT> *File,
     std::vector<ELFShdrTy> *OutputSections = nullptr,
     std::map<std::string, uint32_t> *OutputSectionNameMap = nullptr);

  /// Add a notes section containing the BOLT revision and command line options.
  void addBoltInfoSection();

  /// Update the ELF note section containing the binary build-id to reflect
  /// a new build-id, so tools can differentiate between the old and the
  /// rewritten binary.
  void patchBuildID();

  /// Computes output .debug_line line table offsets for each compile unit,
  /// and updates stmt_list for a corresponding compile unit.
  void updateLineTableOffsets();

  /// Generate new contents for .debug_ranges and .debug_aranges section.
  void finalizeDebugSections();

  /// Patches the binary for DWARF address ranges (e.g. in functions and lexical
  /// blocks) to be updated.
  void updateDWARFAddressRanges();

  /// Rewrite .gdb_index section if present.
  void updateGdbIndexSection();

  /// Patches the binary for an object's address ranges to be updated.
  /// The object can be a anything that has associated address ranges via either
  /// DW_AT_low/high_pc or DW_AT_ranges (i.e. functions, lexical blocks, etc).
  /// \p DebugRangesOffset is the offset in .debug_ranges of the object's
  /// new address ranges in the output binary.
  /// \p Unit Compile uniit the object belongs to.
  /// \p DIE is the object's DIE in the input binary.
  void updateDWARFObjectAddressRanges(const DWARFDie DIE,
                                      uint64_t DebugRangesOffset);

  /// Return file offset corresponding to a given virtual address.
  uint64_t getFileOffsetFor(uint64_t Address) {
    assert(Address >= NewTextSegmentAddress &&
           "address in not in the new text segment");
    return Address - NewTextSegmentAddress + NewTextSegmentOffset;
  }

  /// Return file offset corresponding to a virtual \p Address.
  /// Return 0 if the address has no mapping in the file, including being
  /// part of .bss section.
  uint64_t getFileOffsetForAddress(uint64_t Address) const;

  /// Return true if we will overwrite contents of the section instead
  /// of appending contents to it.
  bool willOverwriteSection(StringRef SectionName);

  /// Construct BinaryFunction object and add it to internal maps.
  BinaryFunction *createBinaryFunction(const std::string &Name,
                                       BinarySection &Section,
                                       uint64_t Address,
                                       uint64_t Size,
                                       bool IsSimple,
                                       uint64_t SymbolSize = 0,
                                       uint16_t Alignment = 0);

public:
  /// When updating debug info, these are the sections we overwrite.
  static constexpr const char *SectionsToOverwrite[] = {
    ".shstrtab",
    ".symtab",
    ".strtab",
    ".debug_aranges",
    ".debug_line",
    ".debug_loc",
    ".debug_ranges",
    ".gdb_index",
  };

private:
  /// Get the contents of the LSDA section for this binary.
  ArrayRef<uint8_t> getLSDAData();

  /// Get the mapped address of the LSDA section for this binary.
  uint64_t getLSDAAddress();

  static const char TimerGroupName[];

  static const char TimerGroupDesc[];

  /// Alignment value used for .eh_frame_hdr.
  static constexpr uint64_t EHFrameHdrAlign = 4;

  // TODO: these are platform (x86, aarch64) specific.
  static constexpr uint64_t PLTSize = 16;
  static constexpr uint16_t PLTAlignment = 16;

  /// An instance of the input binary we are processing, externally owned.
  llvm::object::ELFObjectFileBase *InputFile;

  /// Command line args used to process binary.
  const int Argc;
  const char *const *Argv;

  /// Holds our data aggregator in case user supplied a raw perf data file.
  DataAggregator &DA;

  std::unique_ptr<BinaryContext> BC;
  std::unique_ptr<CFIReaderWriter> CFIRdWrt;

  /// Memory manager for sections and segments. Used to communicate with ORC
  /// among other things.
  std::shared_ptr<ExecutableFileMemoryManager> EFMM;

  std::unique_ptr<orc::SymbolStringPool> SSP;
  std::unique_ptr<orc::ExecutionSession> ES;

  // Run ObjectLinkingLayer() with custom memory manager and symbol resolver.
  std::unique_ptr<orc::RTDyldObjectLinkingLayer> OLT;

  /// Output file where we mix original code from the input binary and
  /// optimized code for selected functions.
  std::unique_ptr<ToolOutputFile> Out;

  /// Offset in the input file where non-allocatable sections start.
  uint64_t FirstNonAllocatableOffset{0};

  /// Information about program header table.
  uint64_t PHDRTableAddress{0};
  uint64_t PHDRTableOffset{0};
  unsigned Phnum{0};

  /// New code segment info.
  uint64_t NewTextSegmentAddress{0};
  uint64_t NewTextSegmentOffset{0};
  uint64_t NewTextSegmentSize{0};

  /// Track next available address for new allocatable sections.
  uint64_t NextAvailableAddress{0};

  /// Entry point in the file (first instructions to be executed).
  uint64_t EntryPoint{0};

  /// Store all non-zero symbols in this map for a quick address lookup.
  std::map<uint64_t, llvm::object::SymbolRef> FileSymRefs;

  /// Store all functions in the binary, sorted by original address.
  std::map<uint64_t, BinaryFunction> BinaryFunctions;

  /// Stores and serializes information that will be put into the .debug_ranges
  /// and .debug_aranges DWARF sections.
  std::unique_ptr<DebugRangesSectionsWriter> RangesSectionsWriter;

  std::unique_ptr<DebugLocWriter> LocationListWriter;

  /// Patchers used to apply simple changes to sections of the input binary.
  /// Maps section name -> patcher.
  std::map<std::string, std::unique_ptr<BinaryPatcher>> SectionPatchers;

  uint64_t NewTextSectionStartAddress{0};

  uint64_t NewTextSectionIndex{0};

  /// Number of local symbols in newly written symbol table.
  uint64_t NumLocalSymbols{0};

  /// Exception handling and stack unwinding information in this binary.
  ErrorOr<BinarySection &> LSDASection{std::errc::bad_address};
  const llvm::DWARFDebugFrame *EHFrame{nullptr};
  ErrorOr<BinarySection &> EHFrameSection{std::errc::bad_address};

  /// .plt section.
  ErrorOr<BinarySection &> PLTSection{std::errc::bad_address};

  /// .got.plt sections.
  ///
  /// Contains jump slots (addresses) indirectly referenced by
  /// instructions in .plt section.
  ErrorOr<BinarySection &> GOTPLTSection{std::errc::bad_address};

  /// .plt.got section (#clowntown).
  ///
  /// A section sometimes  generated by BFD linker.
  ErrorOr<BinarySection &> PLTGOTSection{std::errc::bad_address};

  /// .rela.plt section.
  ///
  /// Contains relocations against .got.plt.
  ErrorOr<BinarySection &> RelaPLTSection{std::errc::bad_address};

  /// .gdb_index section.
  ErrorOr<BinarySection &> GdbIndexSection{std::errc::bad_address};

  /// .note.gnu.build-id section.
  ErrorOr<BinarySection &> BuildIDSection{std::errc::bad_address};

  /// A reference to the build-id bytes in the original binary
  StringRef BuildID;

  uint64_t NewSymTabOffset{0};

  /// Keep track of functions we fail to write in the binary. We need to avoid
  /// rewriting CFI info for these functions.
  std::vector<uint64_t> FailedAddresses;

  /// Keep track of which functions didn't fit in their original space in the
  /// last emission, so that we may either decide to split or not optimize them.
  std::set<uint64_t> LargeFunctions;

  /// Section header string table.
  StringTableBuilder SHStrTab;
  StringPool SHStrTabPool;
  std::vector<PooledStringPtr> AllSHStrTabStrings;

  /// A rewrite of strtab
  std::string NewStrTab;

  static const std::string OrgSecPrefix;

  static const std::string BOLTSecPrefix;

  /// Number of processed to data relocations.  Used to implement the
  /// -max-relocations debugging option.
  uint64_t NumDataRelocations{0};

  friend class RewriteInstanceDiff;

public:

  /// Return binary context.
  const BinaryContext &getBinaryContext() const {
    return *BC;
  }

  /// Return total score of all functions for this instance.
  uint64_t getTotalScore() const {
    return BC->TotalScore;
  }

  /// Return all functions for this rewrite instance.
  const std::map<uint64_t, BinaryFunction> &getFunctions() const {
    return BinaryFunctions;
  }

  /// Return the name of the input file.
  Optional<StringRef> getInputFileName() const {
    if (InputFile)
      return InputFile->getFileName();
    return NoneType();
  }

  /// Set the build-id string if we did not fail to parse the contents of the
  /// ELF note section containing build-id information.
  void parseBuildID();

  /// The build-id is typically a stream of 20 bytes. Return these bytes in
  /// printable hexadecimal form if they are available, or NoneType otherwise.
  Optional<std::string> getPrintableBuildID() const;

  /// Provide an access to the profile data aggregator.
  const DataAggregator &getDataAggregator() const {
    return DA;
  }
};

} // namespace bolt
} // namespace llvm

#endif
