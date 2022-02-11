#include "selector_llvm.hpp"

#include "opto/cfgnode.hpp"
#include "opto/rootnode.hpp"
#include "opto/addnode.hpp"
#include "opto/locknode.hpp"

#include "code_gen/llvmCodeGen.hpp"
#include "adfiles/ad_llvm.hpp"

Selector::Selector(LlvmCodeGen* code_gen, const char* name) :
  Phase(Phase::BlockLayout), 
  _cg(code_gen), _ctx(code_gen->ctx()), _mod(code_gen->mod()), _builder(ctx()),
  _blocks(C->cfg()->number_of_blocks()),
  _pointer_size(mod()->getDataLayout().getPointerSize() * 8), _name(name),
  _is_fast_compression(Universe::narrow_oop_base() == NULL && Universe::narrow_oop_shift() == 0) {}

void Selector::run() {
  create_func();
  prolog();
  select();
  complete_phi_nodes();
}

void Selector::prolog() {
  LlvmStack& stack = cg()->stack();
  llvm::Value* FP = builder().CreateIntrinsic(llvm::Intrinsic::frameaddress, { type(T_ADDRESS) }, { null(T_INT) });
  stack.set_FP(FP);

  _thread = call_C((void*)os::thread_local_storage_at, type(T_ADDRESS), { builder().getInt32(ThreadLocalStorage::thread_index()) });

  size_t alloc_size = cg()->stack().calc_alloc();
  builder().CreateAlloca(type(T_BYTE), builder().getInt32(alloc_size));

  _landing_pad_ty = cg()->has_exceptions() ? llvm::StructType::create({ type(T_ADDRESS), type(T_INT) }) : nullptr;

  Block* block = C->cfg()->get_root_block();
  builder().CreateBr(basic_block(block));
}

void Selector::select() {
  for (size_t i = 0; i < C->unique(); ++i) {
    _cache.push_back(std::make_unique<CacheEntry>());
  }

  _block = C->cfg()->get_root_block();
  builder().SetInsertPoint(basic_block());
  builder().CreateBr(basic_block(block()->non_connector_successor(0)));

  for (size_t i = 1; i < _blocks.length(); ++i) {
    _block = C->cfg()->get_block(i);
    builder().SetInsertPoint(basic_block());
    bool block_addr_set = false;
    for (size_t j = 1; j < block()->number_of_nodes(); ++j) { // skip 0th node: Start or Region
      Node* node = block()->get_node(j);
      if (cg()->has_exceptions() && !block_addr_set && !node->is_Phi()) {
        llvm::Value* id = builder().getInt64(DebugInfo::id(DebugInfo::BlockStart, block()->_pre_order - 1));
        builder().CreateIntrinsic(llvm::Intrinsic::experimental_stackmap, {}, { id, null(T_INT) });
        block_addr_set = true;
      }
      select_node(node);
    }
  }
}

llvm::Value* Selector::tlab_top() {
  llvm::Value* tto = builder().getInt32(in_bytes(JavaThread::tlab_top_offset()));
  return gep(thread(), tto);
}

llvm::Value* Selector::tlab_end() {
  llvm::Value* teo = builder().getInt32(in_bytes(JavaThread::tlab_end_offset()));
  return gep(thread(), teo);
}

llvm::Value* Selector::gep(llvm::Value* base, int offset) {
  return gep(base, builder().getInt64(offset));
}

llvm::Value* Selector::gep(llvm::Value* base, llvm::Value* offset) {
  llvm::Type* ty = base->getType();
  base = builder().CreatePointerCast(base, llvm::Type::getInt8PtrTy(ctx()));
  base = builder().CreateGEP(base, offset);
  return builder().CreatePointerCast(base, ty);
}

llvm::Type* Selector::type(BasicType ty) const {
  switch (ty) {
    case T_BYTE: return llvm::Type::getInt8Ty(_ctx);
    case T_SHORT:
    case T_CHAR: return llvm::Type::getInt16Ty(_ctx);
    case T_INT: 
    case T_NARROWOOP:
    case T_NARROWKLASS: return llvm::Type::getInt32Ty(_ctx);
    case T_LONG: return llvm::Type::getInt64Ty(_ctx);
    case T_FLOAT: return llvm::Type::getFloatTy(_ctx);
    case T_DOUBLE: return llvm::Type::getDoubleTy(_ctx);
    case T_BOOLEAN: return llvm::Type::getInt1Ty(_ctx);
    case T_VOID: return llvm::Type::getVoidTy(_ctx);
    case T_OBJECT:
    case T_METADATA:
    case T_ADDRESS: return llvm::Type::getInt8PtrTy(_ctx);
    default: 
      assert(false, "unable to convert type");
      Unimplemented();
  }
}

void Selector::create_func() {
  llvm::Type* retType = type(C->tf()->return_type());
  const TypeTuple* domain = C->tf()->domain();
  std::vector<llvm::Type*> paramTypes;

  unsigned nf_cnt = 0;
  bool nf_pos_full = false;
  _nf_pos.reserve(NF_REGS);
  for (uint i = TypeFunc::Parms; i < domain->cnt(); ++i) {
    BasicType btype = domain->field_at(i)->basic_type();
    if (btype != T_VOID) {
      if (!nf_pos_full && btype != T_FLOAT && btype != T_DOUBLE) {
        nf_cnt++;
        _nf_pos.push_back(i);
      }
      llvm::Type* ty = type(btype);
      if (nf_cnt == NF_REGS && !nf_pos_full) {
        nf_pos_full = true;
        paramTypes.insert(paramTypes.begin(), ty);
      } else {
        paramTypes.push_back(ty);
      }
    }
  }
  if (nf_cnt != 0 && !nf_pos_full) {
    paramTypes.insert(paramTypes.begin(), type(T_LONG));
  }

  llvm::FunctionType *ftype = llvm::FunctionType::get(retType, paramTypes, false);
  llvm::GlobalValue::LinkageTypes linkage = llvm::GlobalValue::ExternalLinkage;
  _func = llvm::Function::Create(ftype, linkage, 0, _name, _mod);
  func()->addFnAttr("frame-pointer", "all");
  func()->setGC("statepoint-example");
  if (cg()->has_exceptions()) {
    llvm::FunctionCallee pf = mod()->getOrInsertFunction("__gxx_personality_v0", llvm::FunctionType::get(type(T_INT), true));
    func()->setPersonalityFn(llvm::cast<llvm::Constant>(pf.getCallee()));
  }

  create_blocks();
}

std::vector<llvm::Value*> Selector::call_args(MachCallNode* node) {
  const TypeTuple* d = node->tf()->domain();
  std::vector<llvm::Value*> args;
  for (uint i = TypeFunc::Parms; i < d->cnt(); ++i) {
    const Type* at = d->field_at(i);
    if (at->base() == Type::Half) continue;
    llvm::Value* arg = select_node(node->in(i));
    args.push_back(arg);
  }
  return args;
}

void Selector::callconv_adjust(std::vector<llvm::Value*>& args) {
  unsigned nf_cnt = 0;
  for (auto i = args.begin(); i != args.end(); ++i) {
    if ((*i)->getType()->isFloatingPointTy()) continue;
    nf_cnt++;
    if (nf_cnt == NF_REGS) {
      llvm::Value* tmp = *i;
      args.erase(i);
      args.insert(args.begin(), tmp);
      return;
    }
  }
  if (nf_cnt != 0) {
    args.insert(args.begin(), builder().getInt64(0));
  }
}

int Selector::param_to_arg(int param_num) {
  auto it = std::find(_nf_pos.begin(), _nf_pos.end(), param_num);
  if (it != _nf_pos.end()) {
    return (1 + std::distance(_nf_pos.begin(), it)) % NF_REGS;
  }
  const TypeTuple* domain = C->tf()->domain();
  if (domain->cnt() == TypeFunc::Parms)
    return 0;
  int arg_num = (_nf_pos.size() > 0 && _nf_pos.size() < NF_REGS) ? 1 : 0;
  for (uint i = TypeFunc::Parms; i < param_num; ++i) {
    const Type* at = domain->field_at(i);
    if (at->base() == Type::Half) continue;
    arg_num++;
  }
  return arg_num;
}

void Selector::create_blocks() {
  llvm::BasicBlock* entry_block = llvm::BasicBlock::Create(ctx(), "B0", func());
  builder().SetInsertPoint(entry_block);
  std::string b_str = "B";
  for (size_t i = 0; i < C->cfg()->number_of_blocks(); ++i) {
    _blocks.append(llvm::BasicBlock::Create(ctx(), b_str + std::to_string(i + 1), func()));
  }
}

llvm::Value* Selector::select_node(Node* node) {
  CacheEntry* entry = _cache[node->_idx].get();
  if (!entry->hit) {
    entry->val = node->select(this);
    entry->hit = true;
  }
  return entry->val;

}

llvm::Value* Selector::select_address(MachNode *mem_node) {
  const MachOper* mop = mem_node->memory_operand();
  int op_index = MemNode::Address;
  llvm::Value *base, *offset;
  switch (mop->opcode()) {
    case INDIRECT: {
      Node* addr_node = mem_node->in(op_index++);
      assert(addr_node != NULL, "check");
      uint addr_rule = addr_node->is_Mach() ? addr_node->as_Mach()->rule() : _last_Mach_Node;
      if (cg()->cmp_ideal_Opcode(addr_node, Op_AddP)) {
        MachNode* mach_addr = addr_node->as_Mach();
        Node* base_node = mach_addr->in(2);
        if (cg()->cmp_ideal_Opcode(base_node, Op_ConP)) {
          offset = select_oper(base_node->as_Mach()->_opnds[1]);
          base = mach_addr->rule() == addP_rReg_rule
              ? select_node(mach_addr->in(3))
              : select_oper(mach_addr->_opnds[2]);
        } else {
          base = select_node(base_node);
          offset = mach_addr->rule() == addP_rReg_rule
              ? select_node(mach_addr->in(3))
              : select_oper(mach_addr->_opnds[2]);
        }
      } else if (cg()->cmp_ideal_Opcode(addr_node, Op_ConP)) {
        return select_oper(addr_node->as_Mach()->_opnds[1]);
        base = llvm::Constant::getNullValue(llvm::PointerType::getUnqual(offset->getType()));
      } else {
        return select_node(addr_node); 
      }
      if (base->getType()->isIntegerTy() && offset->getType()->isPointerTy()) {
        std::swap(base, offset);
      }
      return gep(base, offset);
    }
    case INDOFFSET: {
      Node* node = mem_node->in(op_index++);
      base = select_node(node);
      return gep(base, mop->constant_disp());
    }
    default: ShouldNotReachHere();
  }
}

llvm::Value* Selector::select_oper(MachOper *oper) {
  const Type* ty = oper->type();
  BasicType bt = ty->basic_type();
  switch (bt) {
  case T_INT: return builder().getInt32(oper->constant());
  case T_LONG: return builder().getInt64(oper->constantL());
  case T_FLOAT: return llvm::ConstantFP::get(
    llvm::Type::getFloatTy(ctx()), llvm::APFloat(oper->constantF()));
  case T_DOUBLE: return llvm::ConstantFP::get(
    llvm::Type::getDoubleTy(ctx()), llvm::APFloat(oper->constantD()));
  case T_ARRAY:
  case T_OBJECT: {
    assert(ty->isa_narrowoop() == NULL, "check");
    llvm::Value* const_oop = get_ptr(*(oop*)ty->is_oopptr()->const_oop()->constant_encoding(), T_OBJECT);
    mark_mptr(const_oop);
    return const_oop;
  }
  case T_METADATA: {
    if (ty->base() == Type::KlassPtr) {
      assert(ty->is_klassptr()->klass()->is_loaded(), "klass not loaded");
      return get_ptr(ty->is_klassptr()->klass()->constant_encoding(), T_METADATA);
    } else {
      return get_ptr(ty->is_metadataptr()->metadata(), T_METADATA);
    }
  }
  case T_NARROWOOP: {
    uint64_t con = ty->is_narrowoop()->get_con();
    if (con != 0) {
      con = *(uint64_t*)con;
      con >>= Universe::narrow_oop_shift();
    }
    llvm::Value* narrow_oop = llvm::ConstantInt::get(type(T_NARROWOOP), con);
    mark_nptr(narrow_oop);
    return narrow_oop;
  }
  case T_NARROWKLASS: {
    uint64_t narrow_klass = ty->is_narrowklass()->get_con();
    narrow_klass >>= Universe::narrow_klass_shift();
    return llvm::ConstantInt::get(type(T_NARROWKLASS), narrow_klass);
  }
  case T_ADDRESS: {
    if (oper->constant() == NULL) return llvm::Constant::getNullValue(type(T_ADDRESS));
    return get_ptr(oper->constant(), T_ADDRESS);
  }
  case T_VOID: return NULL;
  default:
    tty->print_cr("BasicType %d", bt);
    ShouldNotReachHere(); return NULL;
  }
}

llvm::Value* Selector::get_ptr(const void* ptr, llvm::Type* ty) {
  return get_ptr((uint64_t)ptr, ty);
}

llvm::Value* Selector::get_ptr(const void* ptr, BasicType ty) {
  return get_ptr(ptr, type(ty));
}

llvm::Value* Selector::get_ptr(uint64_t ptr, llvm::Type* ty) {
  llvm::IntegerType* intTy = builder().getIntNTy(pointer_size());
  return builder().CreateIntToPtr(llvm::ConstantInt::get(intTy, ptr), ty);
}

llvm::Value* Selector::get_ptr(uint64_t ptr, BasicType ty) {
  return get_ptr(ptr, type(ty));
}

llvm::Value* Selector::select_condition(Node* cmp, llvm::Value* a, llvm::Value* b, bool is_and, bool flt) {
  assert(cmp->outcnt() == 1, "check");

  MachNode* m = cmp->unique_out()->as_Mach();
  int ccode = m->_opnds[1]->ccode();

  assert(!is_and || !flt, "try to and float operands");

  if (flt) {
    switch (ccode) {
    case 0x0: return builder().CreateFCmpUEQ(a, b); // eq
    case 0x1: return builder().CreateFCmpUNE(a, b); // ne
    case 0x2: return builder().CreateFCmpULT(a, b); // lt
    case 0x3: return builder().CreateFCmpULE(a, b); // le
    case 0x4: return builder().CreateFCmpUGT(a, b); // gt
    case 0x5: return builder().CreateFCmpUGE(a, b); // ge
    default: ShouldNotReachHere();
    }
  } else {
    if (is_and) {
      llvm::Value* a_and_b = builder().CreateAnd(a, b);
      llvm::Value* zero = llvm::ConstantInt::get(a->getType(), 0);
      switch (ccode) {
      case 0x0: return builder().CreateICmpEQ(a_and_b, zero); // eq
      case 0x1: return builder().CreateICmpNE(a_and_b, zero); // ne
      case 0x2: return builder().CreateICmpSLT(a_and_b, zero); // lt
      case 0x3: return builder().CreateICmpSLE(a_and_b, zero); // le
      case 0x4: return builder().CreateICmpSGT(a_and_b, zero); // gt
      case 0x5: return builder().CreateICmpSGE(a_and_b, zero); // ge
      default: ShouldNotReachHere();
      }
    } else {
      switch (ccode) {
      case 0x0: return builder().CreateICmpEQ(a, b); // eq
      case 0x1: return builder().CreateICmpNE(a, b); // ne
      case 0x2: return builder().CreateICmpSLT(a, b); // lt
      case 0x3: return builder().CreateICmpSLE(a, b); // le
      case 0x4: return builder().CreateICmpSGT(a, b); // gt
      case 0x5: return builder().CreateICmpSGE(a, b); // ge
      case 0x6: return builder().CreateICmpULT(a, b); // ult
      case 0x7: return builder().CreateICmpULE(a, b); // ule
      case 0x8: return builder().CreateICmpUGT(a, b); // ugt
      case 0x9: return builder().CreateICmpUGE(a, b); // uge
      ///TODO: of & nof
      default: ShouldNotReachHere();
      }
    }
  }
  return NULL;
}

void Selector::select_if(llvm::Value *pred, Node* node) {
  Node* if_node = node->raw_out(0);
  size_t true_idx = 0, false_idx = 1;
  if (if_node->Opcode() == Op_IfFalse) {
    std::swap(true_idx, false_idx);
  } else {
    assert(if_node->Opcode() == Op_IfTrue, "illegal Node type");
  }
  Block* target_block = C->cfg()->get_block_for_node(node->raw_out(true_idx)->raw_out(0));
  Block* fallthr_block = C->cfg()->get_block_for_node(node->raw_out(false_idx)->raw_out(0));
  llvm::BasicBlock* target_bb = basic_block(target_block);
  llvm::BasicBlock* fallthr_bb = basic_block(fallthr_block);
  
  // MachIfNode* if_node = node->as_MachIf();
  // float prob = if_node->_prob;
  // llvm::MDBuilder MDHelper(CGM.getLLVMContext());
  // llvm::MDNode *Weights = MDHelper.createBranchWeights(prob, 1 - prob);
  builder().CreateCondBr(pred, target_bb, fallthr_bb/*, Weights*/);
}

void Selector::replace_return_address(llvm::Value* new_addr) {
  llvm::Value* addr = builder().CreateIntrinsic(llvm::Intrinsic::addressofreturnaddress, { type(T_ADDRESS) }, {});
  store(new_addr, addr);
}

void Selector::mark_inblock() {
  if (cg()->has_exceptions()) {
    uint64_t i = DebugInfo::id(DebugInfo::Inblock);
    llvm::Value* id = builder().getInt64(i);
    builder().CreateIntrinsic(llvm::Intrinsic::experimental_stackmap, {}, { id, null(T_INT) });
  }
}

std::vector<llvm::Type*> Selector::types(const std::vector<llvm::Value*>& v) const {
  std::vector<llvm::Type*> ret;
  ret.reserve(v.size());
  for (llvm::Value* val : v) {
    ret.push_back(val->getType());
  }
  return ret;
}

llvm::CallInst* Selector::call_C(const void* func, llvm::Type* retType, const std::vector<llvm::Value*>& args) {
  llvm::FunctionCallee f = callee(func, retType, args);
  return builder().CreateCall(f, args);
}

llvm::FunctionCallee Selector::callee(const void* func, llvm::Type* retType, const std::vector<llvm::Value*>& args) {
  std::vector<llvm::Type*> paramTypes = types(args);
  llvm::FunctionType* funcTy = llvm::FunctionType::get(retType, paramTypes, false);
  llvm::Value* ptr = get_ptr(func, llvm::PointerType::getUnqual(funcTy));
  return llvm::FunctionCallee(funcTy, ptr);
}

llvm::CallInst* Selector::call(MachCallNode* node, llvm::Type* retType, const std::vector<llvm::Value*>& args) {
  llvm::FunctionCallee f = callee(node->entry_point(), retType, args);
  llvm::Value* callee = f.getCallee();
  ScopeDescriptor& sd = cg()->scope_descriptor();
  ScopeInfo& si = sd.register_scope(node);
  std::vector<llvm::Value*> deopt = sd.stackmap_scope(si);
  llvm::Optional<llvm::ArrayRef<llvm::Value*>> deopt_args(deopt);

  Node* block_end = block()->end();
  CatchNode* catch_node = block_end->isa_Catch();
  DebugInfo::Type ty = DebugInfo::type(si.stackmap_id);
  uint32_t patch_bytes = DebugInfo::patch_bytes(ty);
  Block* next_block = nullptr;

  if (catch_node) {
    uint num_succs = block()->_num_succs;
    std::vector<Block*> handler_blocks;
    handler_blocks.reserve(num_succs);
    for (size_t i = 0; i < num_succs; ++i) {
      CatchProjNode* cp = catch_node->raw_out(i)->as_CatchProj();
      Block* b = C->cfg()->get_block_for_node(cp->raw_out(0));
      if (cp->_con == CatchProjNode::fall_through_index) {
        next_block = b;
      } else {
        handler_blocks.push_back(b);
      }
    }

    llvm::CallInst* ret = nullptr;
    if (next_block) {
      assert(num_succs == 2, "unexpected num_succs");
      llvm::BasicBlock* next_bb = basic_block(next_block);
      llvm::BasicBlock* handler_bb = basic_block(handler_blocks[0]);
      builder().SetInsertPoint(handler_bb);
      llvm::LandingPadInst* lp = builder().CreateLandingPad(landing_pad_ty(), 0);
      lp->setCleanup(true);
      builder().SetInsertPoint(basic_block());
      if (retType->isVoidTy()) {
        builder().CreateGCStatepointInvoke(si.stackmap_id, patch_bytes, callee, next_bb, handler_bb, args, deopt_args, {});
      } else {
        llvm::BasicBlock* result_bb = llvm::BasicBlock::Create(ctx(), basic_block()->getName() + "_result", func());
        llvm::Instruction* statepoint = builder().CreateGCStatepointInvoke(si.stackmap_id, patch_bytes, callee, result_bb, handler_bb, args, deopt_args, {});
        builder().SetInsertPoint(result_bb);
        // no need to mark the inblock with stackmap as it doesn't do anything
        ret = builder().CreateGCResult(statepoint, retType);
        builder().CreateBr(next_bb);
      }
    } else {
      llvm::Instruction* statepoint = builder().CreateGCStatepointCall(si.stackmap_id, patch_bytes, callee, args, deopt_args, {});
      // a faux comparison to attach blocks to the CFG
      llvm::BasicBlock* right_bb = basic_block(handler_blocks[1]);
      for (auto it = handler_blocks.rbegin() + 1; it != handler_blocks.rend() - 1; ++it) {
        right_bb = llvm::BasicBlock::Create(ctx(), basic_block()->getName() + "_handler" + std::to_string(std::distance(handler_blocks.rbegin(), it - 1)), func());
        builder().SetInsertPoint(right_bb);
        mark_inblock();
        llvm::Value* pred = builder().CreateICmpEQ(thread(), null(thread()->getType()));
        builder().CreateCondBr(pred, basic_block(*it), basic_block(*(it - 1)));
      }
      builder().SetInsertPoint(basic_block());
      llvm::Value* pred = builder().CreateICmpEQ(thread(), null(thread()->getType()));
      builder().CreateCondBr(pred, basic_block(handler_blocks[0]), right_bb);
    }
    _handler_table.emplace(block(), std::move(handler_blocks));
    return ret;
  } else {
    llvm::Instruction* statepoint = builder().CreateGCStatepointCall(si.stackmap_id, patch_bytes, callee, args, deopt_args, {});
    next_block = block()->non_connector_successor(0);
    llvm::BasicBlock* next_bb = basic_block(next_block);
    if (node->is_MachCallJava() && !block_end->is_MachReturn() && !block_end->is_MachGoto()) { // ShouldNotReachHere and jmpDir
      builder().CreateBr(next_bb);
    }
    return retType->isVoidTy() ? NULL : builder().CreateGCResult(statepoint, retType);
  }
}

llvm::Value* Selector::load(llvm::Value* addr, BasicType ty) {
  return load(addr, type(ty));
}

llvm::Value* Selector::load(llvm::Value* addr, llvm::Type* ty) {
  addr = builder().CreatePointerCast(addr, llvm::PointerType::getUnqual(ty));
  return builder().CreateLoad(addr);
}

void Selector::store(llvm::Value* value, llvm::Value* addr) {
  addr = builder().CreatePointerCast(addr, llvm::PointerType::getUnqual(value->getType()));
  builder().CreateStore(value, addr);
}

llvm::AtomicCmpXchgInst* Selector::cmpxchg(llvm::Value* addr, llvm::Value* cmp, llvm::Value* val) {
  const llvm::AtomicOrdering succ_ord = llvm::AtomicOrdering::SequentiallyConsistent,
    fail_ord = llvm::AtomicCmpXchgInst::getStrongestFailureOrdering(succ_ord);
  if (cmp->getType()->isPointerTy()) {
    if (val->getType()->isPointerTy()) {
      cmp = builder().CreatePointerCast(cmp, val->getType());
    } else {
      cmp = builder().CreatePtrToInt(cmp, val->getType());
    }
  } else if (val->getType()->isPointerTy()) {
    cmp = builder().CreateIntToPtr(cmp, val->getType());
  }
  addr = builder().CreatePointerCast(addr, llvm::PointerType::getUnqual(cmp->getType()));
  return builder().CreateAtomicCmpXchg(addr, cmp, val, succ_ord, fail_ord);
}

void Selector::mark_mptr(llvm::Value* oop) {
  _oop_info.insert(std::make_pair(oop, std::make_unique<OopInfo>()));
  OopInfo* oop_info_ = oop_info(oop);
  assert(!oop_info_->isNarrowPtr(), "check");
  oop_info_->markManagedPtr(); 
}

void Selector::mark_nptr(llvm::Value* oop) {
  assert(UseCompressedOops, "only with enabled UseCompressedOops");
  if (is_fast_compression()) {
    mark_mptr(oop);
  }
  else {
    _oop_info.insert(std::make_pair(oop, std::make_unique<OopInfo>()));
    OopInfo* oop_info_ = oop_info(oop);
    assert(!oop_info_->isManagedPtr(), "check");
    oop_info_->markNarrowPtr();
  }
}

void Selector::mark_dptr(llvm::Value* ptr, llvm::Value* base) {
  _oop_info.insert(std::make_pair(ptr, std::make_unique<OopInfo>()));
  OopInfo *base_info = oop_info(base), *ptr_info = oop_info(ptr);
  assert(base_info != NULL, "use ManagedPtr instead");
  assert(base_info->isManagedPtr(), "check");
  ptr_info->markDerivedPtr();
}

Node* Selector::find_derived_base(Node* derived) {
  // See if already computed; if so return it
  Node* base;
  if(base = derived_base(derived))
    return base;

  // See if this happens to be a base.
  // NOTE: we use TypePtr instead of TypeOopPtr because we can have
  // pointers derived from NULL!  These are always along paths that
  // can't happen at run-time but the optimizer cannot deduce it so
  // we have to handle it gracefully.
  assert(!derived->bottom_type()->isa_narrowoop() ||
          derived->bottom_type()->make_ptr()->is_ptr()->_offset == 0, "sanity");
  const TypePtr *tj = derived->bottom_type()->isa_ptr();
  // If its an OOP with a non-zero offset, then it is derived.
  if( tj == NULL || tj->_offset == 0 ) {

    _derived_base.insert(std::make_pair(derived, derived));
    return derived;
  }

  // Derived is NULL+offset?  Base is NULL!
  if( derived->is_Con() ) {
    base = C->matcher()->mach_null();
    assert(base != NULL, "verify");
    _derived_base.insert(std::make_pair(derived, base));
    return base;
  }

  // Check for AddP-related opcodes
  if( !derived->is_Phi() ) {
    assert(derived->as_Mach()->ideal_Opcode() == Op_AddP, err_msg("but is: %s", derived->Name()));
    base = derived->in(AddPNode::Base);
    _derived_base.insert(std::make_pair(derived, base));
    return base;
  }

  // Recursively find bases for Phis.
  // First check to see if we can avoid a base Phi here.
  base = find_derived_base(derived->in(1));
  uint i = 2;
  for (; i < derived->req(); i++)
    if ( base != find_derived_base(derived->in(i)))
      break;
  // Went to the end without finding any different bases?
  if( i == derived->req() ) {   // No need for a base Phi here
    _derived_base.insert(std::make_pair(derived, base));
    return base;
  }

  // Now we see we need a base-Phi here to merge the bases
  const Type *t = base->bottom_type();
  base = new (C) PhiNode( derived->in(0), t );
  for( i = 1; i < derived->req(); i++ ) {
    base->init_req(i, find_derived_base(derived->in(i)));
    t = t->meet(base->in(i)->bottom_type());
  }
  base->as_Phi()->set_type(t);

  // Search the current block for an existing base-Phi
  Block *b = C->cfg()->get_block_for_node(derived);
  for( i = 1; i <= b->end_idx(); i++ ) {// Search for matching Phi
    Node *phi = b->get_node(i);
    if( !phi->is_Phi() ) {      // Found end of Phis with no match?
      b->insert_node(base, i); // Must insert created Phi here as base
      C->cfg()->map_node_to_block(base, b);
      break;
    }
    // See if Phi matches.
    uint j;
    for( j = 1; j < base->req(); j++ )
      if( phi->in(j) != base->in(j) &&
          !(phi->in(j)->is_Con() && base->in(j)->is_Con()) ) // allow different NULLs
        break;
    if( j == base->req() ) {    // All inputs match?
      base = phi;               // Then use existing 'phi' and drop 'base'
      break;
    }
  }

  // Cache info for later passes
  _derived_base.insert(std::make_pair(derived, base));
  return base;
}

llvm::Value* Selector::loadKlass_not_null(llvm::Value* obj) {
  llvm::Value* klass_offset = builder().getInt64(oopDesc::klass_offset_in_bytes());
  llvm::Value* addr = gep(obj, klass_offset);
  if (UseCompressedClassPointers) {
    llvm::Value* narrow_klass = load(addr, T_NARROWKLASS);
    return decodeKlass_not_null(narrow_klass);
  }
  return load(addr, T_METADATA);
}

llvm::Value* Selector::decodeKlass_not_null(llvm::Value* narrow_klass) {
  if (!Universe::narrow_klass_shift() && !Universe::narrow_klass_base()) {
     return narrow_klass; 
  }
  narrow_klass = builder().CreateZExt(narrow_klass, builder().getIntNTy(pointer_size()));
  if (Universe::narrow_klass_shift() != 0) {
    llvm::Value* shift = llvm::ConstantInt::get(narrow_klass->getType(), Universe::narrow_klass_shift());
    narrow_klass = builder().CreateShl(narrow_klass, shift);
  }
  if (Universe::narrow_klass_base() != NULL) {
    llvm::Value* base = get_ptr(Universe::narrow_klass_base(), T_METADATA);
    narrow_klass = gep(base, narrow_klass);
  }
  return builder().CreateIntToPtr(narrow_klass, type(T_METADATA));
}

llvm::Value* Selector::decode_heap_oop(llvm::Value* narrow_oop, bool not_null) {
#ifdef ASSERT
  assert (UseCompressedOops, "should be compressed");
  assert (Universe::heap() != NULL, "java heap should be initialized");

  // verify heap base
#endif

  OopInfo* narrow_oop_info = oop_info(narrow_oop);
  if (is_fast_compression()) {
    // 32-bit oops
    assert(narrow_oop_info->isManagedPtr(), "check managed oops flag");
    return narrow_oop;
  } else {
    assert(narrow_oop_info->isNarrowPtr(), "check narrow oops flag");
    llvm::Value* oop;

    assert(Universe::narrow_oop_shift() != 0, "unsupported compression mode");
    narrow_oop = builder().CreateZExt(narrow_oop, builder().getIntNTy(pointer_size()));
    llvm::Value* narrow_oop_shift_ = llvm::ConstantInt::get(narrow_oop->getType(), Universe::narrow_oop_shift());
    llvm::Value* narrow_oop_base_ = get_ptr(Universe::narrow_klass_base(), T_OBJECT);
    if (Universe::narrow_oop_base() == NULL) {
      // Zero-based compressed oops
      oop = builder().CreateShl(narrow_oop, narrow_oop_shift_);
      oop = builder().CreateIntToPtr(oop, type(T_OBJECT));
    } else {
      // Heap-based compressed oops
      if (not_null) {
        oop = builder().CreateShl(narrow_oop, narrow_oop_shift_);
        oop = gep(narrow_oop_base_, oop);
      } else {
        llvm::Value* narrow_zero = llvm::ConstantInt::getNullValue(narrow_oop->getType());
        llvm::Value* zero = llvm::ConstantInt::getNullValue(oop->getType());
        llvm::Value* pred = builder().CreateICmpEQ(narrow_oop, narrow_zero);
        oop = builder().CreateShl(narrow_oop, narrow_oop_shift_);
        oop = gep(narrow_oop_base_, oop);
        oop = builder().CreateSelect(pred, zero, oop);
      }
    }

    DEBUG_ONLY( if (VerifyOops) {/*verify_oop*/} );

    mark_mptr(oop);
    return oop;
  }
}

llvm::Value* Selector::encode_heap_oop(llvm::Value *oop, bool not_null) {
  #ifdef ASSERT
    if (VerifyOops) {
      // verify oops
    }
    if (Universe::narrow_oop_base() != NULL) {
      // also check something
    }
  #endif
  OopInfo* info = oop_info(oop);
  assert(info->isManagedPtr(), "check oop is marked as managed ptr");

  if (is_fast_compression()) {
    // 32-bit oops
    return oop;
  } else {
    llvm::Value* narrow_oop = builder().CreatePtrToInt(oop, builder().getIntNTy(pointer_size()));
    assert(Universe::narrow_oop_shift() != 0, "unsupported compression mode");
    llvm::Value* narrow_oop_shift_ = llvm::ConstantInt::get(narrow_oop->getType(), Universe::narrow_oop_shift());
    llvm::Value* narrow_oop_base_ = builder().getIntN(pointer_size(), (uint64_t)Universe::narrow_klass_base());
    if (Universe::narrow_oop_base() != NULL) {
      // Heap-based compressed oops
      if (not_null) {
        narrow_oop = builder().CreateSub(narrow_oop, narrow_oop_base_);
      } else {
        llvm::Value* zero = llvm::ConstantInt::getNullValue(narrow_oop->getType());
        llvm::Value* pred = builder().CreateICmpEQ(narrow_oop, zero);
        narrow_oop = builder().CreateSub(narrow_oop, narrow_oop_base_);
        narrow_oop = builder().CreateSelect(pred, zero, narrow_oop);
      }
    }
    narrow_oop = builder().CreateAShr(narrow_oop, narrow_oop_shift_);
    narrow_oop = builder().CreateTrunc(narrow_oop, type(T_NARROWOOP));
    mark_nptr(narrow_oop);
    return narrow_oop;
  }
}

void Selector::map_phi_nodes(PhiNode* opto_phi, llvm::PHINode* llvm_phi) {
  _phiNodeMap.push_back(std::make_pair(opto_phi, llvm_phi));
}

void Selector::complete_phi_nodes() {
  for (auto &p : _phiNodeMap) {
    PhiNode* phi_node = p.first;
    llvm::PHINode* phi_inst = p.second;
    Block* phi_block = C->cfg()->get_block_for_node(phi_node);
    RegionNode* phi_region = phi_node->region();
    assert(phi_block->head() == (Node*)phi_region, "check phi block");
    for (uint i = PhiNode::Input; i < phi_node->req(); ++i) {
      Node* case_val = phi_node->in(i);
      Block* case_block = C->cfg()->get_block_for_node(phi_block->pred(i));
      complete_phi_node(case_block, case_val, phi_inst);
    }
  }
}

void Selector::complete_phi_node(Block *case_block, Node* case_val, llvm::PHINode *phi_inst) {
  if (case_block->is_connector()) {
    for (uint i=1; i< case_block->num_preds(); i++) {
      Block *p = C->cfg()->get_block_for_node(case_block->pred(i));
      complete_phi_node(p, case_val, phi_inst);
    }
    return;
  }

  llvm::BasicBlock* case_bb = basic_block(case_block);
  llvm::Value* phi_case = select_node(case_val);
  llvm::Type* phiTy = phi_inst->getType();
  if (phi_case->getType()->isIntegerTy() && phiTy->isPointerTy()) {
    phi_case = builder().CreateIntToPtr(phi_case, phiTy);
    }
  else if (phi_case->getType() != phiTy) {
    llvm::BasicBlock* bb = basic_block(C->cfg()->get_block_for_node(case_val));
    phi_case = llvm::CastInst::CreatePointerCast(phi_case, phiTy);
    llvm::cast<llvm::Instruction>(phi_case)->insertBefore(bb->getTerminator());
  }
  phi_inst->addIncoming(phi_case, case_bb);
}