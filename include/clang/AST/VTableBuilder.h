//===--- VTableBuilder.h - C++ vtable layout builder --------------*- C++ -*-=//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This contains code dealing with generation of the layout of virtual tables.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_AST_VTABLEBUILDER_H
#define LLVM_CLANG_AST_VTABLEBUILDER_H

#include "clang/AST/BaseSubobject.h"
#include "clang/AST/CXXInheritance.h"
#include "clang/AST/GlobalDecl.h"
#include "clang/AST/RecordLayout.h"
#include "clang/Basic/ABI.h"
#include "llvm/ADT/DenseMap.h"
#include <memory>
#include <utility>

namespace clang {
  class CXXRecordDecl;

/// Represents a single component in a vtable.
class VTableComponent {
public:
  enum Kind {
    CK_VCallOffset,
    CK_VBaseOffset,
    CK_OffsetToTop,
    CK_RTTI,
    CK_FunctionPointer,

    /// A pointer to the complete destructor.
    CK_CompleteDtorPointer,

    /// A pointer to the deleting destructor.
    CK_DeletingDtorPointer,

    /// An entry that is never used.
    ///
    /// In some cases, a vtable function pointer will end up never being
    /// called. Such vtable function pointers are represented as a
    /// CK_UnusedFunctionPointer.
    CK_UnusedFunctionPointer
  };

  VTableComponent() = default;

  static VTableComponent MakeVCallOffset(CharUnits Offset) {
    return VTableComponent(CK_VCallOffset, Offset);
  }

  static VTableComponent MakeVCall(const CXXRecordDecl *RD) {
    return VTableComponent(CK_VCallOffset, reinterpret_cast<uintptr_t>(RD));
  }

  static VTableComponent MakeVBaseOffset(CharUnits Offset) {
    return VTableComponent(CK_VBaseOffset, Offset);
  }

  static VTableComponent MakeVBase(const CXXRecordDecl *RD) {
    return VTableComponent(CK_VBaseOffset, reinterpret_cast<uintptr_t>(RD));
  }

  static VTableComponent MakeOffsetToTop(CharUnits Offset) {
    return VTableComponent(CK_OffsetToTop, Offset);
  }

  static VTableComponent MakeRTTI(const CXXRecordDecl *RD) {
    return VTableComponent(CK_RTTI, reinterpret_cast<uintptr_t>(RD));
  }

  static VTableComponent MakeFunction(const CXXMethodDecl *MD) {
    assert(!isa<CXXDestructorDecl>(MD) &&
           "Don't use MakeFunction with destructors!");

    return VTableComponent(CK_FunctionPointer,
                           reinterpret_cast<uintptr_t>(MD));
  }

  static VTableComponent MakeCompleteDtor(const CXXDestructorDecl *DD) {
    return VTableComponent(CK_CompleteDtorPointer,
                           reinterpret_cast<uintptr_t>(DD));
  }

  static VTableComponent MakeDeletingDtor(const CXXDestructorDecl *DD) {
    return VTableComponent(CK_DeletingDtorPointer,
                           reinterpret_cast<uintptr_t>(DD));
  }

  static VTableComponent MakeUnusedFunction(const CXXMethodDecl *MD) {
    assert(!isa<CXXDestructorDecl>(MD) &&
           "Don't use MakeUnusedFunction with destructors!");
    return VTableComponent(CK_UnusedFunctionPointer,
                           reinterpret_cast<uintptr_t>(MD));
  }

  /// Get the kind of this vtable component.
  Kind getKind() const {
    return (Kind)(Value & 0x7);
  }

  CharUnits getVCallOffset() const {
    assert(getKind() == CK_VCallOffset && "Invalid component kind!");

    return getOffset();
  }

  CXXRecordDecl* getVCall() const {
    assert(getKind() == CK_VCallOffset && "Invalid component kind!");

    return reinterpret_cast<CXXRecordDecl *>(getPointer());
  }

  CharUnits getVBaseOffset() const {
    assert(getKind() == CK_VBaseOffset && "Invalid component kind!");

    return getOffset();
  }
  CXXRecordDecl* getVBase() const {
    assert(getKind() == CK_VBaseOffset && "Invalid component kind!");

    return reinterpret_cast<CXXRecordDecl *>(getPointer());
  }

  CharUnits getOffsetToTop() const {
    assert(getKind() == CK_OffsetToTop && "Invalid component kind!");

    return getOffset();
  }

  const CXXRecordDecl *getRTTIDecl() const {
    assert(isRTTIKind() && "Invalid component kind!");
    return reinterpret_cast<CXXRecordDecl *>(getPointer());
  }

  const CXXMethodDecl *getFunctionDecl() const {
    assert(isFunctionPointerKind() && "Invalid component kind!");
    if (isDestructorKind())
      return getDestructorDecl();
    return reinterpret_cast<CXXMethodDecl *>(getPointer());
  }

  const CXXDestructorDecl *getDestructorDecl() const {
    assert(isDestructorKind() && "Invalid component kind!");
    return reinterpret_cast<CXXDestructorDecl *>(getPointer());
  }

  const CXXMethodDecl *getUnusedFunctionDecl() const {
    assert(getKind() == CK_UnusedFunctionPointer && "Invalid component kind!");
    return reinterpret_cast<CXXMethodDecl *>(getPointer());
  }

  bool isDestructorKind() const { return isDestructorKind(getKind()); }

  bool isUsedFunctionPointerKind() const {
    return isUsedFunctionPointerKind(getKind());
  }

  bool isFunctionPointerKind() const {
    return isFunctionPointerKind(getKind());
  }

  bool isRTTIKind() const { return isRTTIKind(getKind()); }

  GlobalDecl getGlobalDecl() const {
    assert(isUsedFunctionPointerKind() &&
           "GlobalDecl can be created only from virtual function");

    auto *DtorDecl = dyn_cast<CXXDestructorDecl>(getFunctionDecl());
    switch (getKind()) {
    case CK_FunctionPointer:
      return GlobalDecl(getFunctionDecl());
    case CK_CompleteDtorPointer:
      return GlobalDecl(DtorDecl, CXXDtorType::Dtor_Complete);
    case CK_DeletingDtorPointer:
      return GlobalDecl(DtorDecl, CXXDtorType::Dtor_Deleting);
    case CK_VCallOffset:
    case CK_VBaseOffset:
    case CK_OffsetToTop:
    case CK_RTTI:
    case CK_UnusedFunctionPointer:
      llvm_unreachable("Only function pointers kinds");
    }
    llvm_unreachable("Should already return");
  }

private:
  static bool isFunctionPointerKind(Kind ComponentKind) {
    return isUsedFunctionPointerKind(ComponentKind) ||
           ComponentKind == CK_UnusedFunctionPointer;
  }
  static bool isUsedFunctionPointerKind(Kind ComponentKind) {
    return ComponentKind == CK_FunctionPointer ||
           isDestructorKind(ComponentKind);
  }
  static bool isDestructorKind(Kind ComponentKind) {
    return ComponentKind == CK_CompleteDtorPointer ||
           ComponentKind == CK_DeletingDtorPointer;
  }
  static bool isRTTIKind(Kind ComponentKind) {
    return ComponentKind == CK_RTTI;
  }

  VTableComponent(Kind ComponentKind, CharUnits Offset) {
    assert((ComponentKind == CK_VCallOffset ||
            ComponentKind == CK_VBaseOffset ||
            ComponentKind == CK_OffsetToTop) && "Invalid component kind!");
    assert(Offset.getQuantity() < (1LL << 56) && "Offset is too big!");
    assert(Offset.getQuantity() >= -(1LL << 56) && "Offset is too small!");

    Value = (uint64_t(Offset.getQuantity()) << 3) | ComponentKind;
  }

  VTableComponent(Kind ComponentKind, uintptr_t Ptr) {
    assert((isRTTIKind(ComponentKind) || isFunctionPointerKind(ComponentKind) || ComponentKind == CK_VCallOffset || ComponentKind == CK_VBaseOffset) &&
           "Invalid component kind!");

    assert((Ptr & 7) == 0 && "Pointer not sufficiently aligned!");

    Value = Ptr | ComponentKind;
  }

  CharUnits getOffset() const {
    assert((getKind() == CK_VCallOffset || getKind() == CK_VBaseOffset ||
            getKind() == CK_OffsetToTop) && "Invalid component kind!");

    return CharUnits::fromQuantity(Value >> 3);
  }

  uintptr_t getPointer() const {
    assert((getKind() == CK_RTTI || getKind() == CK_VCallOffset || getKind() == CK_VBaseOffset || isFunctionPointerKind()) &&
           "Invalid component kind!");

    return static_cast<uintptr_t>(Value & ~7ULL);
  }

  /// The kind is stored in the lower 3 bits of the value. For offsets, we
  /// make use of the facts that classes can't be larger than 2^55 bytes,
  /// so we store the offset in the lower part of the 61 bits that remain.
  /// (The reason that we're not simply using a PointerIntPair here is that we
  /// need the offsets to be 64-bit, even when on a 32-bit machine).
  int64_t Value;
};

class VTableLayout {
public:
  typedef std::pair<uint64_t, ThunkInfo> VTableThunkTy;
  struct AddressPointLocation {
    unsigned VTableIndex, AddressPointIndex;
  };
  typedef llvm::DenseMap<BaseSubobject, AddressPointLocation>
      AddressPointsMapTy;

private:
  // Stores the component indices of the first component of each virtual table in
  // the virtual table group. To save a little memory in the common case where
  // the vtable group contains a single vtable, an empty vector here represents
  // the vector {0}.
  OwningArrayRef<size_t> VTableIndices;

  OwningArrayRef<VTableComponent> VTableComponents;

  /// Contains thunks needed by vtables, sorted by indices.
  OwningArrayRef<VTableThunkTy> VTableThunks;

  /// Address points for all vtables.
  AddressPointsMapTy AddressPoints;

  /// \brief Count of virtual methods for the primary vtable
  uint32_t PrimaryVirtualMethodsCount;

public:
  VTableLayout(ArrayRef<size_t> VTableIndices,
               ArrayRef<VTableComponent> VTableComponents,
               ArrayRef<VTableThunkTy> VTableThunks,
               const AddressPointsMapTy &AddressPoints,
               uint32_t PrimaryVirtualMethodsCount);
  ~VTableLayout();

  ArrayRef<VTableComponent> vtable_components() const {
    return VTableComponents;
  }

  ArrayRef<VTableThunkTy> vtable_thunks() const {
    return VTableThunks;
  }

  AddressPointLocation getAddressPoint(BaseSubobject Base) const {
    assert(AddressPoints.count(Base) && "Did not find address point!");
    return AddressPoints.find(Base)->second;
  }

  const AddressPointsMapTy &getAddressPoints() const {
    return AddressPoints;
  }

  size_t getNumVTables() const {
    if (VTableIndices.empty())
      return 1;
    return VTableIndices.size();
  }

  size_t getVTableOffset(size_t i) const {
    if (VTableIndices.empty()) {
      assert(i == 0);
      return 0;
    }
    return VTableIndices[i];
  }

  size_t getVTableSize(size_t i) const {
    if (VTableIndices.empty()) {
      assert(i == 0);
      return vtable_components().size();
    }

    size_t thisIndex = VTableIndices[i];
    size_t nextIndex = (i + 1 == VTableIndices.size())
                           ? vtable_components().size()
                           : VTableIndices[i + 1];
    return nextIndex - thisIndex;
  }

  uint32_t getPrimaryVirtualMethodsCount() const {
    return PrimaryVirtualMethodsCount;
  }
};

class VTableContextBase {
public:
  typedef SmallVector<ThunkInfo, 1> ThunkInfoVectorTy;

  bool isMicrosoft() const { return IsMicrosoftABI; }

  virtual ~VTableContextBase() {}

protected:
  typedef llvm::DenseMap<const CXXMethodDecl *, ThunkInfoVectorTy> ThunksMapTy;

  /// Contains all thunks that a given method decl will need.
  ThunksMapTy Thunks;

  /// Compute and store all vtable related information (vtable layout, vbase
  /// offset offsets, thunks etc) for the given record decl.
  virtual void computeVTableRelatedInformation(const CXXRecordDecl *RD) = 0;

  VTableContextBase(bool MS) : IsMicrosoftABI(MS) {}

public:
  virtual const ThunkInfoVectorTy *getThunkInfo(GlobalDecl GD) {
    const CXXMethodDecl *MD = cast<CXXMethodDecl>(GD.getDecl()->getCanonicalDecl());
    computeVTableRelatedInformation(MD->getParent());

    // This assumes that all the destructors present in the vtable
    // use exactly the same set of thunks.
    ThunksMapTy::const_iterator I = Thunks.find(MD);
    if (I == Thunks.end()) {
      // We did not find a thunk for this method.
      return nullptr;
    }

    return &I->second;
  }

  bool IsMicrosoftABI;
};

class ItaniumVTableContext : public VTableContextBase {
private:

  /// Contains the index (relative to the vtable address point)
  /// where the function pointer for a virtual function is stored.
  typedef llvm::DenseMap<GlobalDecl, int64_t> MethodVTableIndicesTy;
  MethodVTableIndicesTy MethodVTableIndices;

  typedef llvm::DenseMap<const CXXRecordDecl *,
                         std::unique_ptr<const VTableLayout>>
      VTableLayoutMapTy;
  VTableLayoutMapTy VTableLayouts;

  typedef std::pair<const CXXRecordDecl *,
                    const CXXRecordDecl *> ClassPairTy;

  /// vtable offsets for offsets of virtual bases of a class.
  ///
  /// Contains the vtable offset (relative to the address point) in chars
  /// where the offsets for virtual bases of a class are stored.
  typedef llvm::DenseMap<ClassPairTy, CharUnits>
    VirtualBaseClassOffsetOffsetsMapTy;
  VirtualBaseClassOffsetOffsetsMapTy VirtualBaseClassOffsetOffsets;

  void computeVTableRelatedInformation(const CXXRecordDecl *RD) override;

public:
  ItaniumVTableContext(ASTContext &Context);
  ~ItaniumVTableContext() override;

  const VTableLayout &getVTableLayout(const CXXRecordDecl *RD) {
    computeVTableRelatedInformation(RD);
    assert(VTableLayouts.count(RD) && "No layout for this record decl!");

    return *VTableLayouts[RD];
  }

  std::unique_ptr<VTableLayout> createConstructionVTableLayout(
      const CXXRecordDecl *MostDerivedClass, CharUnits MostDerivedClassOffset,
      bool MostDerivedClassIsVirtual, const CXXRecordDecl *LayoutClass);

  /// Locate a virtual function in the vtable.
  ///
  /// Return the index (relative to the vtable address point) where the
  /// function pointer for the given virtual function is stored.
  uint64_t getMethodVTableIndex(GlobalDecl GD);

  /// Return the offset in chars (relative to the vtable address point) where
  /// the offset of the virtual base that contains the given base is stored,
  /// otherwise, if no virtual base contains the given class, return 0.
  ///
  /// Base must be a virtual base class or an unambiguous base.
  CharUnits getVirtualBaseOffsetOffset(const CXXRecordDecl *RD,
                                       const CXXRecordDecl *VBase);

  static bool classof(const VTableContextBase *VT) {
    return !VT->isMicrosoft();
  }
};

/// Holds information about the inheritance path to a virtual base or function
/// table pointer.  A record may contain as many vfptrs or vbptrs as there are
/// base subobjects.
struct VPtrInfo {
  typedef SmallVector<const CXXRecordDecl *, 1> BasePath;

  VPtrInfo(const CXXRecordDecl *RD)
      : ObjectWithVPtr(RD), IntroducingObject(RD), NextBaseToMangle(RD) {}

  /// This is the most derived class that has this vptr at offset zero. When
  /// single inheritance is used, this is always the most derived class. If
  /// multiple inheritance is used, it may be any direct or indirect base.
  const CXXRecordDecl *ObjectWithVPtr;

  /// This is the class that introduced the vptr by declaring new virtual
  /// methods or virtual bases.
  const CXXRecordDecl *IntroducingObject;

  /// IntroducingObject is at this offset from its containing complete object or
  /// virtual base.
  CharUnits NonVirtualOffset;

  /// The bases from the inheritance path that got used to mangle the vbtable
  /// name.  This is not really a full path like a CXXBasePath.  It holds the
  /// subset of records that need to be mangled into the vbtable symbol name in
  /// order to get a unique name.
  BasePath MangledPath;

  /// The next base to push onto the mangled path if this path is ambiguous in a
  /// derived class.  If it's null, then it's already been pushed onto the path.
  const CXXRecordDecl *NextBaseToMangle;

  /// The set of possibly indirect vbases that contain this vbtable.  When a
  /// derived class indirectly inherits from the same vbase twice, we only keep
  /// vtables and their paths from the first instance.
  BasePath ContainingVBases;

  /// This holds the base classes path from the complete type to the first base
  /// with the given vfptr offset, in the base-to-derived order.  Only used for
  /// vftables.
  BasePath PathToIntroducingObject;

  /// Static offset from the top of the most derived class to this vfptr,
  /// including any virtual base offset.  Only used for vftables.
  CharUnits FullOffsetInMDC;

  /// The vptr is stored inside the non-virtual component of this virtual base.
  const CXXRecordDecl *getVBaseWithVPtr() const {
    return ContainingVBases.empty() ? nullptr : ContainingVBases.front();
  }
};

typedef SmallVector<std::unique_ptr<VPtrInfo>, 2> VPtrInfoVector;

/// All virtual base related information about a given record decl.  Includes
/// information on all virtual base tables and the path components that are used
/// to mangle them.
struct VirtualBaseInfo {
  /// A map from virtual base to vbtable index for doing a conversion from the
  /// the derived class to the a base.
  llvm::DenseMap<const CXXRecordDecl *, unsigned> VBTableIndices;

  /// Information on all virtual base tables used when this record is the most
  /// derived class.
  VPtrInfoVector VBPtrPaths;
};

struct MethodVFTableLocation {
  /// If nonzero, holds the vbtable index of the virtual base with the vfptr.
  uint64_t VBTableIndex;

  /// If nonnull, holds the last vbase which contains the vfptr that the
  /// method definition is adjusted to.
  const CXXRecordDecl *VBase;

  /// This is the offset of the vfptr from the start of the last vbase, or the
  /// complete type if there are no virtual bases.
  CharUnits VFPtrOffset;

  /// Method's index in the vftable.
  uint64_t Index;

  MethodVFTableLocation()
      : VBTableIndex(0), VBase(nullptr), VFPtrOffset(CharUnits::Zero()),
        Index(0) {}

  MethodVFTableLocation(uint64_t VBTableIndex, const CXXRecordDecl *VBase,
                        CharUnits VFPtrOffset, uint64_t Index)
      : VBTableIndex(VBTableIndex), VBase(VBase), VFPtrOffset(VFPtrOffset),
        Index(Index) {}

  bool operator<(const MethodVFTableLocation &other) const {
    if (VBTableIndex != other.VBTableIndex) {
      assert(VBase != other.VBase);
      return VBTableIndex < other.VBTableIndex;
    }
    return std::tie(VFPtrOffset, Index) <
           std::tie(other.VFPtrOffset, other.Index);
  }
};

class MicrosoftVTableContext : public VTableContextBase {
public:

private:
  ASTContext &Context;

  typedef llvm::DenseMap<GlobalDecl, MethodVFTableLocation>
    MethodVFTableLocationsTy;
  MethodVFTableLocationsTy MethodVFTableLocations;

  typedef llvm::DenseMap<const CXXRecordDecl *, std::unique_ptr<VPtrInfoVector>>
      VFPtrLocationsMapTy;
  VFPtrLocationsMapTy VFPtrLocations;

  typedef std::pair<const CXXRecordDecl *, CharUnits> VFTableIdTy;
  typedef llvm::DenseMap<VFTableIdTy, std::unique_ptr<const VTableLayout>>
      VFTableLayoutMapTy;
  VFTableLayoutMapTy VFTableLayouts;

  llvm::DenseMap<const CXXRecordDecl *, std::unique_ptr<VirtualBaseInfo>>
      VBaseInfo;

  void enumerateVFPtrs(const CXXRecordDecl *ForClass, VPtrInfoVector &Result);

  void computeVTableRelatedInformation(const CXXRecordDecl *RD) override;

  void dumpMethodLocations(const CXXRecordDecl *RD,
                           const MethodVFTableLocationsTy &NewMethods,
                           raw_ostream &);

  const VirtualBaseInfo &
  computeVBTableRelatedInformation(const CXXRecordDecl *RD);

  void computeVTablePaths(bool ForVBTables, const CXXRecordDecl *RD,
                          VPtrInfoVector &Paths);

public:
  MicrosoftVTableContext(ASTContext &Context)
      : VTableContextBase(/*MS=*/true), Context(Context) {}

  ~MicrosoftVTableContext() override;

  const VPtrInfoVector &getVFPtrOffsets(const CXXRecordDecl *RD);

  const VTableLayout &getVFTableLayout(const CXXRecordDecl *RD,
                                       CharUnits VFPtrOffset);

  MethodVFTableLocation getMethodVFTableLocation(GlobalDecl GD);

  const ThunkInfoVectorTy *getThunkInfo(GlobalDecl GD) override {
    // Complete destructors don't have a slot in a vftable, so no thunks needed.
    if (isa<CXXDestructorDecl>(GD.getDecl()) &&
        GD.getDtorType() == Dtor_Complete)
      return nullptr;
    return VTableContextBase::getThunkInfo(GD);
  }

  /// Returns the index of VBase in the vbtable of Derived.
  /// VBase must be a morally virtual base of Derived.
  /// The vbtable is an array of i32 offsets.  The first entry is a self entry,
  /// and the rest are offsets from the vbptr to virtual bases.
  unsigned getVBTableIndex(const CXXRecordDecl *Derived,
                           const CXXRecordDecl *VBase);

  const VPtrInfoVector &enumerateVBTables(const CXXRecordDecl *RD);

  static bool classof(const VTableContextBase *VT) { return VT->isMicrosoft(); }
};

} // namespace clang

#endif
