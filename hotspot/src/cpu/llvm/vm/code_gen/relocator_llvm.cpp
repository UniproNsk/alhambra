#include "relocator_llvm.hpp"

#include <algorithm>

#include "utilities/debug.hpp"
#include "asm/macroAssembler.hpp"

#include "llvmCodeGen.hpp"

RelocationHolder CallReloc::getHolder() {
  switch (kind()) {
    case HotspotRelocInfo::RelocOptVirtualCall: return opt_virtual_call_Relocation::spec();
    case HotspotRelocInfo::RelocStaticCall:     return static_call_Relocation::spec();
    case HotspotRelocInfo::RelocRuntimeCall:    return runtime_call_Relocation::spec();
    default: ShouldNotReachHere();     return Relocation::spec_simple(relocInfo::none);
  }
}

RelocationHolder VirtualCallReloc::getHolder() {
  assert(_IC_addr, "address not set");
  return virtual_call_Relocation::spec(_IC_addr);
}

RelocationHolder ConstReloc::getHolder() {
  assert(_con_addr, "address not set");
  return internal_word_Relocation::spec(_con_addr);
}

RelocationHolder InternalReloc::getHolder() {
  return Relocation::spec_simple(relocInfo::internal_word_type);
}

void LlvmRelocator::add(DebugInfo* di, size_t offset) {
  Reloc* rel;
  switch (di->type()) {
    case DebugInfo::DynamicCall: {
      rel = new VirtualCallReloc(offset);
      break;
    }
    case DebugInfo::StaticCall: {
      ciMethod* method = di->asCall()->scope_info->cjn->_method;
      bool is_runtime = method == NULL;
      HotspotRelocInfo reloc_info;
      if (is_runtime) {
        reloc_info = HotspotRelocInfo::RelocRuntimeCall;
      } else if (method->is_static()) {
        reloc_info = HotspotRelocInfo::RelocStaticCall;
      } else {
        reloc_info = HotspotRelocInfo::RelocOptVirtualCall;
      }
      rel = new CallReloc(reloc_info, offset);
      break;
    }
    case DebugInfo::Rethrow: rel = new CallReloc(HotspotRelocInfo::RelocRuntimeCall, offset); break;
    case DebugInfo::Oop: rel = new OopReloc(offset, di->asOop()->con); break;
    case DebugInfo::OrigPC: rel = new InternalReloc(offset); break;
    default: ShouldNotReachHere();
  }
  relocs.push_back(rel);
}

void LlvmRelocator::add_float(size_t offset, float con) {
  FloatReloc* rel = new FloatReloc(offset, con);
  relocs.push_back(rel);
}

void LlvmRelocator::add_double(size_t offset, double con) {
  DoubleReloc* rel = new DoubleReloc(offset, con);
  relocs.push_back(rel);
}

void LlvmRelocator::apply_relocs(MacroAssembler* masm) {
  CodeSection *insts = cg()->cb()->insts(), *consts = cg()->cb()->consts();
  assert(masm->code_section() == insts, "sanity check");
  std::sort(relocs.begin(), relocs.end(),
    [](const Reloc* a, const Reloc* b) { return a->offset() < b->offset(); });
  for (Reloc* rel : relocs) {
    ConstReloc* c_rel;
    CallReloc* call_rel;
    if (c_rel = rel->asConstReloc()) {
      address con_addr = nullptr;
      FloatReloc* f_rel;
      DoubleReloc* d_rel;
      OopReloc* oop_rel;
      if (f_rel = rel->asFloatReloc()) {
        con_addr = masm->float_constant(f_rel->con());
      } else if (d_rel = rel->asDoubleReloc()) {
        con_addr = masm->double_constant(d_rel->con());
      } else if (oop_rel = rel->asOopReloc()) {
        int oop_index = masm->oop_recorder()->allocate_oop_index((jobject)oop_rel->con());
        con_addr = masm->address_constant((address)oop_rel->con());
        cg()->cb()->consts()->relocate(con_addr, oop_Relocation::spec(oop_index));
      }
      c_rel->set_con_addr(con_addr);
    } else if (call_rel = rel->asCallReloc()) {
      VirtualCallReloc* v_rel;
      if (v_rel = rel->asVirtualCallReloc()) {
        v_rel->set_IC_addr(masm->addr_at(v_rel->offset() - NativeMovConstReg::instruction_size));
      }
    }
    address addr = masm->addr_at(rel->offset());
    masm->code_section()->relocate(addr, rel->getHolder());
  }
}