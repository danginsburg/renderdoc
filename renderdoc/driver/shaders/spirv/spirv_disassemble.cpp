/******************************************************************************
 * The MIT License (MIT)
 * 
 * Copyright (c) 2015 Baldur Karlsson
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include "common/common.h"

#include "serialise/serialiser.h"

#include "spirv_common.h"

#include <utility>
using std::pair;
using std::make_pair;

#undef min
#undef max

#include "3rdparty/glslang/SPIRV/spirv.h"
#include "3rdparty/glslang/SPIRV/GLSL450Lib.h"
#include "3rdparty/glslang/glslang/Public/ShaderLang.h"

const char *GLSL_STD_450_names[GLSL_STD_450::Count] = {0};

// list of known generators, just for kicks
struct { uint32_t magic; const char *name; } KnownGenerators[] = {
	{ 0x051a00bb, "glslang" },
};

template<typename EnumType>
static string OptionalFlagString(EnumType e)
{
	return (int)e	? "[" + ToStr::Get(e) + "]" : "";
}

struct SPVInstruction;

struct SPVDecoration
{
	SPVDecoration() : decoration(spv::DecorationPrecisionLow), val(0) {}

	spv::Decoration decoration;

	uint32_t val;
};

struct SPVExtInstSet
{
	SPVExtInstSet() : instructions(NULL) {}

	string setname;
	const char **instructions;
};

struct SPVEntryPoint
{
	SPVEntryPoint() : func(0), model(spv::ExecutionModelVertex) {}

	// entry point will come before declaring instruction,
	// so we reference the function by ID
	uint32_t func;
	spv::ExecutionModel model;
};

struct SPVTypeData
{
	SPVTypeData() :
		baseType(NULL), storage(spv::StorageClassUniformConstant),
		bitCount(32), vectorSize(1), matrixSize(1), arraySize(1) {}

	enum
	{
		eVoid,
		eBool,
		eFloat,
		eSInt,
		eUInt,
		eBasicCount,

		eVector,
		eMatrix,
		eArray,
		ePointer,
		eCompositeCount,

		eFunction,

		eStruct,
		eSampler,
		eFilter,

	} type;

	SPVTypeData *baseType;

	string name;
	
	vector<SPVDecoration> decorations;

	// struct/function
	vector< pair<SPVTypeData *, string> > children;

	// pointer
	spv::StorageClass storage;

	// ints and floats
	uint32_t bitCount;

	uint32_t vectorSize;
	uint32_t matrixSize;
	uint32_t arraySize;
};

struct SPVOperation
{
	SPVOperation() : type(NULL), complexity(0) {}

	SPVTypeData *type;

	// OpLoad/OpStore/OpCopyMemory
	spv::MemoryAccessMask access;
	
	// OpAccessChain
	vector<uint32_t> literals;

	// this is modified on the fly, it's used as a measure of whether we
	// can combine multiple statements into one line when displaying the
	// disassembly.
	int complexity;

	// arguments always reference IDs that already exist (branch/flow
	// control type statements aren't SPVOperations)
	vector<SPVInstruction*> arguments;
};

struct SPVConstant
{
	SPVConstant() : type(NULL), u64(0) {}

	SPVTypeData *type;
	union
	{
		uint64_t u64;
		uint32_t u32;
		uint16_t u16;
		int64_t i64;
		int32_t i32;
		int16_t i16;
		float f;
		double d;
	};

	vector<SPVConstant *> children;
};

struct SPVVariable
{
	SPVVariable() : type(NULL), storage(spv::StorageClassUniformConstant), initialiser(NULL) {}

	SPVTypeData *type;
	spv::StorageClass storage;
	SPVConstant *initialiser;
};

struct SPVFlowControl
{
	SPVFlowControl() : selControl(spv::SelectionControlMaskNone), condition(NULL) {}

	union
	{
		spv::SelectionControlMask selControl;
		spv::LoopControlMask loopControl;
	};
	
	SPVInstruction *condition;

	// branch weights or switch cases
	vector<uint32_t> literals;

	// flow control can reference future IDs, so we index
	vector<uint32_t> targets;
};

struct SPVBlock
{
	SPVBlock() : mergeFlow(NULL), exitFlow(NULL) {}
	
	vector<SPVInstruction *> instructions;

	SPVInstruction *mergeFlow;
	SPVInstruction *exitFlow;
};

struct SPVFunction
{
	SPVFunction() : retType(NULL), funcType(NULL), control(spv::FunctionControlMaskNone) {}

	SPVTypeData *retType;
	SPVTypeData *funcType;
	spv::FunctionControlMask control;
	vector<SPVInstruction *> blocks;
	vector<SPVInstruction *> variables;
};

struct SPVInstruction
{
	SPVInstruction()
	{
		opcode = spv::OpNop;
		id = 0;

		ext = NULL;
		entry = NULL;
		op = NULL;
		flow = NULL;
		type = NULL;
		func = NULL;
		block = NULL;
		constant = NULL;
		var = NULL;

		line = -1;

		source.col = source.line = 0;
	}

  ~SPVInstruction()
	{
		SAFE_DELETE(ext);
		SAFE_DELETE(entry);
		SAFE_DELETE(op);
		SAFE_DELETE(flow);
		SAFE_DELETE(type);
		SAFE_DELETE(func);
		SAFE_DELETE(block);
		SAFE_DELETE(constant);
		SAFE_DELETE(var);
	}

  int indentChange()
  {
    return 0;
  }

  bool useIndent()
  {
    return opcode != spv::OpLabel;
  }
	
  spv::Op opcode;
	uint32_t id;

	// line number in disassembly (used for stepping when debugging)
	int line;

	struct { string filename; uint32_t line; uint32_t col; } source;

  string str;

	vector<SPVDecoration> decorations;

	// zero or one of these pointers might be set
	SPVExtInstSet *ext; // this ID is an extended instruction set
	SPVEntryPoint *entry; // this ID is an entry point
	SPVOperation *op; // this ID is the result of an operation
	SPVFlowControl *flow; // this is a flow control operation (no ID)
	SPVTypeData *type; // this ID names a type
	SPVFunction *func; // this ID names a function
	SPVBlock *block; // this is the ID of a label
	SPVConstant *constant; // this ID is a constant value
	SPVVariable *var; // this ID is a variable
};

struct SPVModule
{
	SPVModule()
	{
		shadType = eSPIRVInvalid;
		moduleVersion = 0;
		generator = 0;
		source.lang = spv::SourceLanguageUnknown; source.ver = 0;
		model.addr = spv::AddressingModelLogical; model.mem = spv::MemoryModelSimple;
	}

	~SPVModule()
	{
		for(size_t i=0; i < operations.size(); i++)
			delete operations[i];
		operations.clear();
	}

	SPIRVShaderStage shadType;
	uint32_t moduleVersion;
	uint32_t generator;
	struct { spv::SourceLanguage lang; uint32_t ver; } source;
	struct { spv::AddressingModel addr; spv::MemoryModel mem; } model;

	vector<SPVInstruction*> operations; // all operations (including those that don't generate an ID)

	vector<SPVInstruction*> ids; // pointers indexed by ID

	vector<SPVInstruction*> entries; // entry points
	vector<SPVInstruction*> globals; // global variables
	vector<SPVInstruction*> funcs; // functions

	string Disassemble()
	{
		string disasm;

		// temporary function until we build our own structure from the SPIR-V
		const char *header[] = {
			"Vertex Shader",
			"Tessellation Control Shader",
			"Tessellation Evaluation Shader",
			"Geometry Shader",
			"Fragment Shader",
			"Compute Shader",
		};

		disasm = header[(int)shadType];
		disasm += " SPIR-V:\n\n";

		const char *gen = "Unrecognised";

		for(size_t i=0; i < ARRAY_COUNT(KnownGenerators); i++) if(KnownGenerators[i].magic == generator)	gen = KnownGenerators[i].name;

		disasm += StringFormat::Fmt("Version %u, Generator %08x (%s)\n", moduleVersion, generator, gen);
		disasm += StringFormat::Fmt("IDs up to {%u}\n", (uint32_t)ids.size());

		disasm += "\n";

		// add global props
		//
		// add global variables
		//
		// add func pre-declares(?)
		//
		// for each func:
		//   declare instructino list
		//   push variable declares
		//
		//   for each block:
		//     for each instruction:
		//       add to list
		//       if instruction takes params:
		//         if params instructions complexity are low enough
		//           take param instructions out of list
		//           incr. this instruction's complexity
		//           mark params to be melded
		//           aggressively meld function call parameters, remove variable declares
		//
		//   do magic secret sauce to analyse ifs and loops
		//
		//   for instructions in list:
		//     mark line num to all 'child' instructions, for stepping
		//     output combined line
		//     if instruction pair is goto then label, skip
		//
		//

		return disasm;
	}
};

void DisassembleSPIRV(SPIRVShaderStage shadType, const vector<uint32_t> &spirv, string &disasm)
{
#if defined(RELEASE)
	return;
#else
	if(shadType >= eSPIRVInvalid)
		return;

	SPVModule module;

	module.shadType = shadType;

	if(spirv[0] != (uint32_t)spv::MagicNumber)
	{
		disasm = StringFormat::Fmt("Unrecognised magic number %08x", spirv[0]);
		return;
	}
	
	module.moduleVersion = spirv[1];
	module.generator = spirv[2];
	module.ids.resize(spirv[3]);

	uint32_t idbound = spirv[3];

	RDCASSERT(spirv[4] == 0);

	SPVFunction *curFunc = NULL;
	SPVBlock *curBlock = NULL;

	size_t it = 5;
	while(it < spirv.size())
	{
		uint16_t WordCount = spirv[it]>>16;

		module.operations.push_back(new SPVInstruction());
		SPVInstruction &op = *module.operations.back();

		op.opcode = spv::Op(spirv[it]&0xffff);

		switch(op.opcode)
		{
			//////////////////////////////////////////////////////////////////////
			// 'Global' opcodes
			case spv::OpSource:
			{
				module.source.lang = spv::SourceLanguage(spirv[it+1]);
				module.source.ver = spirv[it+2];
				break;
			}
			case spv::OpMemoryModel:
			{
				module.model.addr = spv::AddressingModel(spirv[it+1]);
				module.model.mem = spv::MemoryModel(spirv[it+2]);
				break;
			}
			case spv::OpEntryPoint:
			{
				op.entry = new SPVEntryPoint();
				op.entry->func = spirv[it+2];
				op.entry->model = spv::ExecutionModel(spirv[it+1]);
				module.entries.push_back(&op);
				break;
			}
			case spv::OpExtInstImport:
			{
				op.ext = new SPVExtInstSet();
				op.ext->setname = (const char *)&spirv[it+2];
				op.ext->instructions = NULL;

				if(op.ext->setname == "GLSL.std.450")
				{
					op.ext->instructions = GLSL_STD_450_names;

					if(GLSL_STD_450_names[0] == NULL)
						GLSL_STD_450::GetDebugNames(GLSL_STD_450_names);
				}

				op.id = spirv[it+1];
				module.ids[spirv[it+1]] = &op;
				break;
			}
			case spv::OpString:
			{
				op.str = (const char *)&spirv[it+2];

				op.id = spirv[it+1];
				module.ids[spirv[it+1]] = &op;
				break;
			}
			//////////////////////////////////////////////////////////////////////
			// Type opcodes
			case spv::OpTypeVoid:
			{
				op.type = new SPVTypeData();
				op.type->type = SPVTypeData::eVoid;
				
				op.id = spirv[it+1];
				module.ids[spirv[it+1]] = &op;
				break;
			}
			case spv::OpTypeBool:
			{
				op.type = new SPVTypeData();
				op.type->type = SPVTypeData::eBool;
				
				op.id = spirv[it+1];
				module.ids[spirv[it+1]] = &op;
				break;
			}
			case spv::OpTypeInt:
			{
				op.type = new SPVTypeData();
				op.type->type = spirv[it+3] ? SPVTypeData::eSInt : SPVTypeData::eUInt;
				op.type->bitCount = spirv[it+2];
				
				op.id = spirv[it+1];
				module.ids[spirv[it+1]] = &op;
				break;
			}
			case spv::OpTypeFloat:
			{
				op.type = new SPVTypeData();
				op.type->type = SPVTypeData::eFloat;
				op.type->bitCount = spirv[it+2];
				
				op.id = spirv[it+1];
				module.ids[spirv[it+1]] = &op;
				break;
			}
			case spv::OpTypeVector:
			{
				op.type = new SPVTypeData();
				op.type->type = SPVTypeData::eVector;

				SPVInstruction *baseTypeInst = module.ids[spirv[it+2]];
				RDCASSERT(baseTypeInst && baseTypeInst->type);

				op.type->baseType = baseTypeInst->type;
				op.type->vectorSize = spirv[it+3];
				
				op.id = spirv[it+1];
				module.ids[spirv[it+1]] = &op;
				break;
			}
			case spv::OpTypeArray:
			{
				op.type = new SPVTypeData();
				op.type->type = SPVTypeData::eArray;

				SPVInstruction *baseTypeInst = module.ids[spirv[it+2]];
				RDCASSERT(baseTypeInst && baseTypeInst->type);

				op.type->baseType = baseTypeInst->type;
				op.type->arraySize = spirv[it+3];
				
				op.id = spirv[it+1];
				module.ids[spirv[it+1]] = &op;
				break;
			}
			case spv::OpTypeStruct:
			{
				op.type = new SPVTypeData();
				op.type->type = SPVTypeData::eStruct;

				for(int i=2; i < WordCount; i++)
				{
					SPVInstruction *memberInst = module.ids[spirv[it+i]];
					RDCASSERT(memberInst && memberInst->type);

					// names might come later from OpMemberName instructions
					op.type->children.push_back(make_pair(memberInst->type, ""));
				}
				
				op.id = spirv[it+1];
				module.ids[spirv[it+1]] = &op;
				break;
			}
			case spv::OpTypePointer:
			{
				op.type = new SPVTypeData();
				op.type->type = SPVTypeData::ePointer;

				SPVInstruction *baseTypeInst = module.ids[spirv[it+3]];
				RDCASSERT(baseTypeInst && baseTypeInst->type);

				op.type->baseType = baseTypeInst->type;
				op.type->storage = spv::StorageClass(spirv[it+2]);
				
				op.id = spirv[it+1];
				module.ids[spirv[it+1]] = &op;
				break;
			}
			case spv::OpTypeFunction:
			{
				op.type = new SPVTypeData();
				op.type->type = SPVTypeData::eFunction;

				for(int i=3; i < WordCount; i++)
				{
					SPVInstruction *argInst = module.ids[spirv[it+i]];
					RDCASSERT(argInst && argInst->type);

					// function parameters have no name
					op.type->children.push_back(make_pair(argInst->type, ""));
				}

				SPVInstruction *baseTypeInst = module.ids[spirv[it+2]];
				RDCASSERT(baseTypeInst && baseTypeInst->type);

				// return type
				op.type->baseType = baseTypeInst->type;
				
				op.id = spirv[it+1];
				module.ids[spirv[it+1]] = &op;
				break;
			}
			//////////////////////////////////////////////////////////////////////
			// Constants
			case spv::OpConstant:
			{
				SPVInstruction *typeInst = module.ids[spirv[it+1]];
				RDCASSERT(typeInst && typeInst->type);

				op.constant = new SPVConstant();
				op.constant->type = typeInst->type;

				op.constant->u32 = spirv[it+3];

				if(WordCount > 3)
				{
					// only handle 32-bit or 64-bit constants
					RDCASSERT(WordCount <= 4);

					uint64_t lo = spirv[it+3];
					uint64_t hi = spirv[it+4];

					op.constant->u64 = lo | (hi<<32);
				}
				
				op.id = spirv[it+2];
				module.ids[spirv[it+2]] = &op;
				break;
			}
			case spv::OpConstantComposite:
			{
				SPVInstruction *typeInst = module.ids[spirv[it+1]];
				RDCASSERT(typeInst && typeInst->type);

				op.constant = new SPVConstant();
				op.constant->type = typeInst->type;

				for(int i=3; i < WordCount; i++)
				{
					SPVInstruction *constInst = module.ids[spirv[it+i]];
					RDCASSERT(constInst && constInst->constant);

					op.constant->children.push_back(constInst->constant);
				}
				
				op.id = spirv[it+2];
				module.ids[spirv[it+2]] = &op;

				break;
			}
			//////////////////////////////////////////////////////////////////////
			// Functions
			case spv::OpFunction:
			{
				SPVInstruction *retTypeInst = module.ids[spirv[it+1]];
				RDCASSERT(retTypeInst && retTypeInst->type);

				SPVInstruction *typeInst = module.ids[spirv[it+4]];
				RDCASSERT(typeInst && typeInst->type);

				op.func = new SPVFunction();
				op.func->retType = retTypeInst->type;
				op.func->funcType = typeInst->type;
				op.func->control = spv::FunctionControlMask(spirv[it+3]);
				
				op.id = spirv[it+2];
				module.ids[spirv[it+2]] = &op;

				curFunc = op.func;

				break;
			}
			case spv::OpFunctionEnd:
			{
				curFunc = NULL;
				break;
			}
			//////////////////////////////////////////////////////////////////////
			// Variables
			case spv::OpVariable:
			{
				SPVInstruction *typeInst = module.ids[spirv[it+1]];
				RDCASSERT(typeInst && typeInst->type);

				op.var = new SPVVariable();
				op.var->type = typeInst->type;
				op.var->storage = spv::StorageClass(spirv[it+3]);

				if(WordCount > 4)
				{
					SPVInstruction *initInst = module.ids[spirv[it+4]];
					RDCASSERT(initInst && initInst->constant);
					op.var->initialiser = initInst->constant;
				}

				if(curFunc) curFunc->variables.push_back(&op);
				else module.globals.push_back(&op);
				
				op.id = spirv[it+2];
				module.ids[spirv[it+2]] = &op;
				break;
			}
			//////////////////////////////////////////////////////////////////////
			// Branching/flow control
			case spv::OpLabel:
			{
				op.block = new SPVBlock();

				RDCASSERT(curFunc);

				curFunc->blocks.push_back(&op);
				curBlock = op.block;
				
				curBlock->instructions.push_back(&op);
				
				op.id = spirv[it+1];
				module.ids[spirv[it+1]] = &op;
				break;
			}
			case spv::OpKill:
			case spv::OpUnreachable:
			case spv::OpReturn:
			{
				op.flow = new SPVFlowControl();
				
				curBlock->exitFlow = &op;
				curBlock->instructions.push_back(&op);
				curBlock = NULL;
				break;
			}
			case spv::OpReturnValue:
			{
				op.flow = new SPVFlowControl();
				
				op.flow->targets.push_back(spirv[it+1]);

				curBlock->exitFlow = &op;
				curBlock->instructions.push_back(&op);
				curBlock = NULL;
				break;
			}
			case spv::OpBranch:
			{
				op.flow = new SPVFlowControl();
				
				op.flow->targets.push_back(spirv[it+1]);
				
				curBlock->exitFlow = &op;
				curBlock->instructions.push_back(&op);
				curBlock = NULL;
				break;
			}
			case spv::OpBranchConditional:
			{
				op.flow = new SPVFlowControl();
				
				SPVInstruction *condInst = module.ids[spirv[it+1]];
				RDCASSERT(condInst);

				op.flow->condition = condInst;
				op.flow->targets.push_back(spirv[it+2]);
				op.flow->targets.push_back(spirv[it+3]);

				if(WordCount == 6)
				{
					op.flow->literals.push_back(spirv[it+4]);
					op.flow->literals.push_back(spirv[it+5]);
				}
				
				curBlock->exitFlow = &op;
				curBlock->instructions.push_back(&op);
				curBlock = NULL;
				break;
			}
			case spv::OpSelectionMerge:
			{
				op.flow = new SPVFlowControl();
				
				op.flow->targets.push_back(spirv[it+1]);
				op.flow->selControl = spv::SelectionControlMask(spirv[it+2]);
				
				curBlock->mergeFlow = &op;
				curBlock->instructions.push_back(&op);
				break;
			}
			case spv::OpLoopMerge:
			{
				op.flow = new SPVFlowControl();
				
				op.flow->targets.push_back(spirv[it+1]);
				op.flow->loopControl = spv::LoopControlMask(spirv[it+2]);
				
				curBlock->mergeFlow = &op;
				curBlock->instructions.push_back(&op);
				break;
			}
			//////////////////////////////////////////////////////////////////////
			// Operations with special parameters
			case spv::OpAccessChain:
			{
				SPVInstruction *typeInst = module.ids[spirv[it+1]];
				RDCASSERT(typeInst && typeInst->type);

				op.op = new SPVOperation();
				op.op->type = typeInst->type;
				
				SPVInstruction *structInst = module.ids[spirv[it+3]];
				RDCASSERT(structInst);

				op.op->arguments.push_back(structInst);
	
				for(int i=4; i < WordCount; i++)
					op.op->literals.push_back(spirv[it+i]);
				
				op.id = spirv[it+2];
				module.ids[spirv[it+2]] = &op;
				
				curBlock->instructions.push_back(&op);
				break;
			}
			case spv::OpLoad:
			{
				SPVInstruction *typeInst = module.ids[spirv[it+1]];
				RDCASSERT(typeInst && typeInst->type);

				op.op = new SPVOperation();
				op.op->type = typeInst->type;
				
				SPVInstruction *ptrInst = module.ids[spirv[it+3]];
				RDCASSERT(ptrInst);

				op.op->arguments.push_back(ptrInst);

				op.op->access = spv::MemoryAccessMaskNone;

				for(int i=4; i < WordCount; i++)
				{
					if(i == WordCount-1)
					{
						op.op->access = spv::MemoryAccessMask(spirv[it+i]);
					}
					else
					{
						uint32_t lit = spirv[it+i];
						// don't understand what these literals are - seems like OpAccessChain handles
						// struct member/array access so it doesn't seem to be for array indices
						RDCBREAK(); 
					}
				}
				
				op.id = spirv[it+2];
				module.ids[spirv[it+2]] = &op;
				
				curBlock->instructions.push_back(&op);
				break;
			}
			case spv::OpStore:
			case spv::OpCopyMemory:
			{
				op.op = new SPVOperation();
				op.op->type = NULL;
				
				SPVInstruction *ptrInst = module.ids[spirv[it+1]];
				RDCASSERT(ptrInst);
				
				SPVInstruction *valInst = module.ids[spirv[it+2]];
				RDCASSERT(valInst);

				op.op->arguments.push_back(ptrInst);
				op.op->arguments.push_back(valInst);

				op.op->access = spv::MemoryAccessMaskNone;

				for(int i=3; i < WordCount; i++)
				{
					if(i == WordCount-1)
					{
						op.op->access = spv::MemoryAccessMask(spirv[it+i]);
					}
					else
					{
						uint32_t lit = spirv[it+i];
						// don't understand what these literals are - seems like OpAccessChain handles
						// struct member/array access so it doesn't seem to be for array indices
						RDCBREAK(); 
					}
				}
				
				curBlock->instructions.push_back(&op);
				break;
			}
			//////////////////////////////////////////////////////////////////////
			// Easy to handle opcodes with just some number of ID parameters
			case spv::OpIAdd:
			case spv::OpIMul:
			case spv::OpFAdd:
			case spv::OpFMul:
			case spv::OpSLessThan:
			case spv::OpExtInst:
			{
				SPVInstruction *typeInst = module.ids[spirv[it+1]];
				RDCASSERT(typeInst && typeInst->type);

				op.op = new SPVOperation();
				op.op->type = typeInst->type;
				
				for(int i=3; i < WordCount; i++)
				{
					SPVInstruction *argInst = module.ids[spirv[it+i]];
					RDCASSERT(argInst);

					op.op->arguments.push_back(argInst);
				}
				
				op.id = spirv[it+2];
				module.ids[spirv[it+2]] = &op;
				
				curBlock->instructions.push_back(&op);
				break;
			}
			default:
				break;
		}

		it += WordCount;
	}

	// second pass now that we have all ids set up, apply decorations/names/etc
	it = 5;
	while(it < spirv.size())
	{
		uint16_t WordCount = spirv[it]>>16;
		spv::Op op = spv::Op(spirv[it]&0xffff);

		switch(op)
		{
			case spv::OpName:
			{
				SPVInstruction *varInst = module.ids[spirv[it+1]];
				RDCASSERT(varInst);
				if(varInst->type)
					varInst->type->name = (const char *)&spirv[it+2];
				else
					varInst->str = (const char *)&spirv[it+2];
				break;
			}
			case spv::OpMemberName:
			{
				SPVInstruction *varInst = module.ids[spirv[it+1]];
				RDCASSERT(varInst && varInst->type && varInst->type->type == SPVTypeData::eStruct);
				uint32_t memIdx = spirv[it+2];
				RDCASSERT(memIdx < varInst->type->children.size());
				varInst->type->children[memIdx].second = (const char *)&spirv[it+3];
				break;
			}
			case spv::OpLine:
			{
				SPVInstruction *varInst = module.ids[spirv[it+1]];
				RDCASSERT(varInst);

				SPVInstruction *fileInst = module.ids[spirv[it+2]];
				RDCASSERT(fileInst);

				varInst->source.filename = fileInst->str;
				varInst->source.line = spirv[it+3];
				varInst->source.col = spirv[it+4];
				break;
			}
			case spv::OpDecorate:
			{
				SPVInstruction *inst = module.ids[spirv[it+1]];
				RDCASSERT(inst);

				SPVDecoration d;
				d.decoration = spv::Decoration(spirv[it+2]);
				
				// TODO this isn't enough for all decorations
				RDCASSERT(WordCount <= 3);
				if(WordCount > 2)
					d.val = spirv[it+3];

				inst->decorations.push_back(d);
				break;
			}
			case spv::OpMemberDecorate:
			case spv::OpGroupDecorate:
			case spv::OpGroupMemberDecorate:
			case spv::OpDecorationGroup:
				// TODO
				RDCBREAK();
				break;
		}

		it += WordCount;
	}

	vector<string> resultnames;
	resultnames.resize(idbound);
	
	vector< pair<uint32_t, const char **> > extensionSets;
	extensionSets.reserve(2);
	
	vector< pair<uint32_t, string> > decorations;

	// needs to be fleshed out, but this is enough for now
	enum BaseType
	{
		eSPIRVTypeVoid,
		eSPIRVTypeBool,
		eSPIRVTypeFloat, // assuming floats are all 32-bit
		eSPIRVTypeSInt32, // assuming ints are signed or unsigned 32-bit
		eSPIRVTypeUInt32,
	};

	vector<BaseType> typeinfo;
	typeinfo.resize(idbound);

	vector<uint32_t> values;
	values.resize(idbound);

	// complete hack
	vector<const char *> membernames;

	// fetch names and things to be used in the second pass.
	// could get away with one pass, just need to detect when function
	// declarations/definitions start to fill out unnamed IDs. Or wrap
	// it all up and give them anonymous <id> names on the fly (more
	// likely).
	it = 5;
	while(it < spirv.size())
	{
		uint16_t WordCount = spirv[it]>>16;
		spv::Op OpCode = spv::Op(spirv[it]&0xffff);

		if(OpCode == spv::OpName)
		{
			resultnames[ spirv[it+1] ] = (const char *)&spirv[it+2];
		}
		else if(OpCode == spv::OpLabel)
		{
				resultnames[spirv[it+1]] = StringFormat::Fmt("Label%u", spirv[it+1]);
		}
		else if(OpCode == spv::OpMemberName)
		{
			uint32_t id = spirv[it+1];
			uint32_t memberIdx = spirv[it+2];
			const char *memberName = (const char *)&spirv[it+3];

			// COMPLETE hack
			membernames.resize( RDCMAX(membernames.size(), (size_t)memberIdx+1) );
			membernames[memberIdx] = memberName;
		}
		else if(OpCode == spv::OpDecorate)
		{
			uint32_t target = spirv[it+1];
			spv::Decoration decoration = spv::Decoration(spirv[it+2]);

			// TODO: decoration parameters here...

			decorations.push_back( make_pair(target, ToStr::Get(decoration)) );
		}
		else if(OpCode == spv::OpTypeVoid)
		{
			resultnames[ spirv[it+1] ] = "void";
			typeinfo[ spirv[it+1] ] = eSPIRVTypeVoid;
		}
		else if(OpCode == spv::OpTypeBool)
		{
			resultnames[ spirv[it+1] ] = "bool";
			typeinfo[ spirv[it+1] ] = eSPIRVTypeBool;
		}
		else if(OpCode == spv::OpTypeInt)
		{
			resultnames[ spirv[it+1] ] = "int";
			RDCASSERT( spirv[it+2] == 32 );
			typeinfo[ spirv[it+1] ] = spirv[it+3] ? eSPIRVTypeSInt32 : eSPIRVTypeUInt32;
		}
		else if(OpCode == spv::OpTypeFloat)
		{
			resultnames[ spirv[it+1] ] = "float";
			RDCASSERT( spirv[it+2] == 32 );
			typeinfo[ spirv[it+1] ] = eSPIRVTypeFloat;
		}
		else if(OpCode == spv::OpTypeVector)
		{
			resultnames[ spirv[it+1] ] = StringFormat::Fmt("%s%u", resultnames[ spirv[it+2] ].c_str(), spirv[it+3]);
			typeinfo[ spirv[it+1] ] = typeinfo[ spirv[it+2] ];
		}
		else if(OpCode == spv::OpTypeArray)
		{
			resultnames[ spirv[it+1] ] = StringFormat::Fmt("%s[%u]", resultnames[ spirv[it+2] ].c_str(), values[ spirv[it+3] ]);
			typeinfo[ spirv[it+1] ] = typeinfo[ spirv[it+2] ];
		}
		else if(OpCode == spv::OpTypeStruct)
		{
			resultnames[ spirv[it+1] ] = "struct"; // don't need to decode this at all, we're not going to use the type info
		}
		else if(OpCode == spv::OpTypePointer)
		{
			uint32_t id = spirv[it+1];
			spv::StorageClass storage = spv::StorageClass(spirv[it+2]);
			uint32_t baseType = spirv[it+3];

			// bit specific for where we need it (variable declarations), but all this data will be properly parsed & stored
			// so each instruction can use it as it wishes
			resultnames[id] = resultnames[baseType] + "*";
		}
		else if(OpCode == spv::OpTypeFunction)
		{
			// this name will just be used for the arguments in the function definition string, don't need to keep the type info
			// or print the return type anywhere (as it must match the return type in the function definition opcode)
			string args = "";

			for(int i=3; i < WordCount; i++)
			{
				uint32_t typeId = spirv[it+i];

				args += resultnames[typeId];

				if(i+1 < WordCount)
					args += ", ";
			}

			if(args.empty()) args = "void";

			resultnames[ spirv[it+1] ] = args;
		}
		else if(OpCode == spv::OpConstant)
		{
			uint32_t typeId = spirv[it+1];
			uint32_t id = spirv[it+2];

			// hack - assuming only up to 32-bit values
			values[id] = spirv[it+3];

			BaseType type = typeinfo[typeId];
			string lit = "";

			if(type == eSPIRVTypeBool)
				lit += values[id] ? "true" : "false";
			else if(type == eSPIRVTypeFloat)
				lit += StringFormat::Fmt("%f", *(float*)&values[id]);
			else if(type == eSPIRVTypeSInt32)
				lit += StringFormat::Fmt("%d", *(int32_t*)&values[id]);
			else if(type == eSPIRVTypeUInt32)
				lit += StringFormat::Fmt("%u", values[id]);

			resultnames[id] = StringFormat::Fmt("%s(%s)", resultnames[typeId].c_str(), lit.c_str());
		}
		else if(OpCode == spv::OpConstantComposite)
		{
			uint32_t typeId = spirv[it+1];
			uint32_t id = spirv[it+2];
			
			BaseType type = typeinfo[typeId];
			string lits = "";

			for(int i=3; i < WordCount; i++)
			{
				uint32_t val = spirv[it+i];

				if(type == eSPIRVTypeBool)
					lits += values[val] ? "true" : "false";
				else if(type == eSPIRVTypeFloat)
					lits += StringFormat::Fmt("%f", *(float*)&values[val]);
				else if(type == eSPIRVTypeSInt32)
					lits += StringFormat::Fmt("%d", *(int32_t*)&values[val]);
				else if(type == eSPIRVTypeUInt32)
					lits += StringFormat::Fmt("%u", values[val]);

				if(i+1 < WordCount)
					lits += ", ";
			}

			resultnames[id] = StringFormat::Fmt("%s(%s)", resultnames[typeId].c_str(), lits.c_str());
		}
		
		it += WordCount;
	}

	for(size_t i=0; i < resultnames.size(); i++)
		if(resultnames[i].empty())
			resultnames[i] = StringFormat::Fmt("{%d}", i);

	const size_t tabSize = 2;

	string indent;
	indent.reserve(tabSize*6);

	string funcname;
	vector<uint32_t> flowstack;

	bool variables = false;

	it = 5;
	while(it < spirv.size())
	{
		uint16_t WordCount = spirv[it]>>16;
		spv::Op OpCode = spv::Op(spirv[it]&0xffff);

		string body;
		bool silent = false;

		switch(OpCode)
		{
			case spv::OpSource:
			{
				body = StringFormat::Fmt("Source %s %d", ToStr::Get(spv::SourceLanguage(spirv[it+1])).c_str(), spirv[it+2]);
				break;
			}
			case spv::OpExtInstImport:
			{
				resultnames[ spirv[it+1] ] = (char *)&spirv[it+2];
				body = StringFormat::Fmt("ExtInstImport %s", (char *)&spirv[it+2]);

				if(resultnames[ spirv[it+1] ] == "GLSL.std.450")
				{
					extensionSets.push_back( make_pair(spirv[it+1], GLSL_STD_450_names) );

					if(GLSL_STD_450_names[0] == NULL)
						GLSL_STD_450::GetDebugNames(GLSL_STD_450_names);
				}

				break;
			}
			case spv::OpMemoryModel:
			{
				body = StringFormat::Fmt("MemoryModel %s Addressing, %s Memory model",
					ToStr::Get(spv::AddressingModel(spirv[it+1])).c_str(),
					ToStr::Get(spv::MemoryModel(spirv[it+2])).c_str());
				break;
			}
			case spv::OpEntryPoint:
			{
				body = StringFormat::Fmt("EntryPoint = %s (%s)",
					resultnames[ spirv[it+2] ].c_str(),
					ToStr::Get(spv::ExecutionModel(spirv[it+1])).c_str());
				break;
			}
			case spv::OpVariable:
			{
				if(!variables)
				{
					variables = true;
					disasm += "\n";
				}

				uint32_t retType = spirv[it+1];
				uint32_t resultId = spirv[it+2];
				spv::StorageClass control = spv::StorageClass(spirv[it+3]);

				uint32_t initializer = ~0U;
				if(WordCount > 4)
					initializer = spirv[it+4];

				string decorationsStr = "";

				for(auto it=decorations.begin(); it != decorations.end(); ++it)
				{
					if(it->first == resultId)
					{
						decorationsStr += it->second + " ";
					}
				}

				body = StringFormat::Fmt("%s%s %s %s",
					decorationsStr.c_str(),
					ToStr::Get(control).c_str(),
					resultnames[ retType ].c_str(),
					resultnames[ resultId ].c_str());

				if(initializer < idbound)
					body += StringFormat::Fmt(" = %s", resultnames[initializer].c_str());
				break;
			}
			case spv::OpFunction:
			{
				uint32_t retType = spirv[it+1];
				uint32_t resultId = spirv[it+2];
				spv::FunctionControlMask control = spv::FunctionControlMask(spirv[it+3]);
				uint32_t funcType = spirv[it+4];

				// add an extra newline
				disasm += "\n";
				body = StringFormat::Fmt("%s %s(%s) %s {",
					resultnames[retType].c_str(),
					resultnames[resultId].c_str(),
					resultnames[funcType].c_str(),
					OptionalFlagString(control).c_str());

				funcname = resultnames[resultId];

				break;
			}
			case spv::OpFunctionEnd:
			{
				body = StringFormat::Fmt("} // end of %s", funcname.c_str());
				funcname = "";
				indent.resize(indent.size() - tabSize);
				break;
			}
			case spv::OpAccessChain:
			{
				uint32_t retType = spirv[it+1];
				uint32_t resultId = spirv[it+2];
				uint32_t base = spirv[it+3];
	
				body = StringFormat::Fmt("%s %s = %s",
					resultnames[retType].c_str(),
					resultnames[resultId].c_str(),
					resultnames[base].c_str());
				
				// this is a complete and utter hack
				for(int i=4; i < WordCount; i++)
				{
					if(i == 4 && values[spirv[it+4]] < membernames.size())
						body += StringFormat::Fmt(".%s", membernames[ values[spirv[it+4]] ]);
					else
						body += StringFormat::Fmt("[%s]", resultnames[ spirv[it+i] ].c_str());
				}

				break;
			}
			case spv::OpLoad:
			{
				uint32_t retType = spirv[it+1];
				uint32_t resultId = spirv[it+2];
				uint32_t pointer = spirv[it+3];

				spv::MemoryAccessMask access = spv::MemoryAccessMaskNone;

				for(int i=4; i < WordCount; i++)
				{
					if(i == WordCount-1)
					{
						access = spv::MemoryAccessMask(spirv[it+i]);
					}
					else
					{
						uint32_t lit = spirv[it+i];
						// don't understand what these literals are - seems like OpAccessChain handles
						// struct member/array access so it doesn't seem to be for array indices
						RDCBREAK(); 
					}
				}

				body = StringFormat::Fmt("%s %s = Load(%s) %s",
					resultnames[retType].c_str(),
					resultnames[resultId].c_str(),
					resultnames[pointer].c_str(),
					OptionalFlagString(access).c_str());
				break;
			}
			case spv::OpStore:
			case spv::OpCopyMemory:
			{
				uint32_t pointer = spirv[it+1];
				uint32_t object = spirv[it+2];

				spv::MemoryAccessMask access = spv::MemoryAccessMaskNone;

				for(int i=3; i < WordCount; i++)
				{
					if(i == WordCount-1)
					{
						access = spv::MemoryAccessMask(spirv[it+i]);
					}
					else
					{
						uint32_t lit = spirv[it+i];
						// don't understand what these literals are - seems like OpAccessChain handles
						// struct member/array access so it doesn't seem to be for array indices
						RDCBREAK(); 
					}
				}

				if(OpCode == spv::OpStore)
					body = StringFormat::Fmt("Store(%s) = %s %s",
						resultnames[ pointer ].c_str(),
						resultnames[ object ].c_str(),
						OptionalFlagString(access).c_str());
				if(OpCode == spv::OpCopyMemory)
					body = StringFormat::Fmt("Copy(%s) = Load(%s) %s",
						resultnames[ pointer ].c_str(),
						resultnames[ object ].c_str(),
						OptionalFlagString(access).c_str());
				break;
			}
			case spv::OpName:
			case spv::OpMemberName:
			case spv::OpDecorate:
			case spv::OpConstant:
			case spv::OpConstantComposite:
			case spv::OpTypeVoid:
			case spv::OpTypeBool:
			case spv::OpTypeInt:
			case spv::OpTypeFloat:
			case spv::OpTypeVector:
			case spv::OpTypePointer:
			case spv::OpTypeArray:
			case spv::OpTypeStruct:
			case spv::OpTypeFunction:
			{
				silent = true;
				break;
			}
			case spv::OpIAdd:
			case spv::OpIMul:
			case spv::OpFAdd:
			case spv::OpFMul:
			case spv::OpSLessThan:
			{
				char op = '?';
				switch(OpCode)
				{
					case spv::OpIAdd:
					case spv::OpFAdd:
						op = '+';
						break;
					case spv::OpIMul:
					case spv::OpFMul:
						op = '*';
						break;
					case spv::OpSLessThan:
						op = '<';
						break;
					default:
						break;
				}

				uint32_t retType = spirv[it+1];
				uint32_t result = spirv[it+2];
				uint32_t a = spirv[it+3];
				uint32_t b = spirv[it+4];

				body = StringFormat::Fmt("%s %s = %s %c %s",
					resultnames[ retType ].c_str(),
					resultnames[ result ].c_str(),
					resultnames[ a ].c_str(),
					op,
					resultnames[ b ].c_str());
				break;
			}
			case spv::OpExtInst:
			{
				uint32_t retType = spirv[it+1];
				uint32_t result = spirv[it+2];
				uint32_t extset = spirv[it+3];
				uint32_t instruction = spirv[it+4];

				string instructionName = "";

				for(auto ext = extensionSets.begin(); ext != extensionSets.end(); ++ext)
					if(ext->first == extset)
						instructionName = ext->second[instruction];

				if(instructionName.empty())
					instructionName = StringFormat::Fmt("Unknown%u", instruction);

				string args = "";

				for(int i=5; i < WordCount; i++)
				{
					args += resultnames[ spirv[it+i] ];

					if(i+1 < WordCount)
						args += ", ";
				}
				
				body = StringFormat::Fmt("%s %s = %s::%s(%s)",
					resultnames[ retType ].c_str(),
					resultnames[ result ].c_str(),
					resultnames[ extset ].c_str(),
					instructionName.c_str(),
					args.c_str());

				break;
			}
			case spv::OpReturn:
			{
				body = "Return";
				break;
			}
			case spv::OpSelectionMerge:
			{
				uint32_t mergeLabel = spirv[it+1];
				spv::SelectionControlMask control = spv::SelectionControlMask(spirv[it+2]);

				flowstack.push_back(mergeLabel);
				
				body = StringFormat::Fmt("SelectionMerge %s %s", resultnames[ mergeLabel ].c_str(), OptionalFlagString(control).c_str());
				break;
			}
			case spv::OpLoopMerge:
			{
				uint32_t mergeLabel = spirv[it+1];
				spv::LoopControlMask control = spv::LoopControlMask(spirv[it+2]);

				flowstack.push_back(mergeLabel);
				
				body = StringFormat::Fmt("LoopMerge %s %s", resultnames[ mergeLabel ].c_str(), OptionalFlagString(control).c_str());
				break;
			}
			case spv::OpBranch:
			{
				body = StringFormat::Fmt("goto %s", resultnames[ spirv[it+1] ].c_str());
				break;
			}
			case spv::OpBranchConditional:
			{
				uint32_t condition = spirv[it+1];
				uint32_t truelabel = spirv[it+2];
				uint32_t falselabel = spirv[it+3];

				if(WordCount == 4)
				{
					body = StringFormat::Fmt("if(%s) goto %s, else goto %s",
						resultnames[condition].c_str(),
						resultnames[truelabel].c_str(),
						resultnames[falselabel].c_str());
				}
				else
				{
					uint32_t weightA = spirv[it+4];
					uint32_t weightB = spirv[it+5];

					float a = float(weightA)/float(weightA+weightB);
					float b = float(weightB)/float(weightA+weightB);

					a *= 100.0f;
					b *= 100.0f;

					body = StringFormat::Fmt("if(%s) goto %.2f%% %s, else goto %.2f%% %s",
						a,
						resultnames[condition].c_str(),
						resultnames[truelabel].c_str(),
						b,
						resultnames[falselabel].c_str());
				}

				break;
			}
			case spv::OpLabel:
			{
				body = resultnames[spirv[it+1]] + ":";

				if(!flowstack.empty() && flowstack.back() == spirv[it+1])
					indent.resize(indent.size() - tabSize);
				break;
			}
			default:
			{
				body = "!" + ToStr::Get(OpCode);
				for(uint16_t i=1; i < WordCount; i++)
				{
					if(spirv[it+i] <= idbound)
						body += StringFormat::Fmt(" %u%s", spirv[it+i], i+1 < WordCount ? "," : "");
					else
						body += StringFormat::Fmt(" %#x%s", spirv[it+i], i+1 < WordCount ? "," : "");
				}
				break;
			}
		}
		
		if(!silent)
			disasm += StringFormat::Fmt("%s%s\n", indent.c_str(), body.c_str());
		
		// post printing operations
		switch(OpCode)
		{
			case spv::OpFunction:
				indent.insert(indent.end(), tabSize, ' ');
				break;
			case spv::OpSelectionMerge:
			case spv::OpLoopMerge:
				indent.insert(indent.end(), tabSize, ' ');
				break;
			default:
				break;
		}

		it += WordCount;
	}

	return;
#endif
}

template<>
string ToStrHelper<false, spv::Op>::Get(const spv::Op &el)
{
	switch(el)
	{
		case spv::OpNop:                                      return "Nop";
		case spv::OpSource:                                   return "Source";
		case spv::OpSourceExtension:                          return "SourceExtension";
		case spv::OpExtension:                                return "Extension";
		case spv::OpExtInstImport:                            return "ExtInstImport";
		case spv::OpMemoryModel:                              return "MemoryModel";
		case spv::OpEntryPoint:                               return "EntryPoint";
		case spv::OpExecutionMode:                            return "ExecutionMode";
		case spv::OpTypeVoid:                                 return "TypeVoid";
		case spv::OpTypeBool:                                 return "TypeBool";
		case spv::OpTypeInt:                                  return "TypeInt";
		case spv::OpTypeFloat:                                return "TypeFloat";
		case spv::OpTypeVector:                               return "TypeVector";
		case spv::OpTypeMatrix:                               return "TypeMatrix";
		case spv::OpTypeSampler:                              return "TypeSampler";
		case spv::OpTypeFilter:                               return "TypeFilter";
		case spv::OpTypeArray:                                return "TypeArray";
		case spv::OpTypeRuntimeArray:                         return "TypeRuntimeArray";
		case spv::OpTypeStruct:                               return "TypeStruct";
		case spv::OpTypeOpaque:                               return "TypeOpaque";
		case spv::OpTypePointer:                              return "TypePointer";
		case spv::OpTypeFunction:                             return "TypeFunction";
		case spv::OpTypeEvent:                                return "TypeEvent";
		case spv::OpTypeDeviceEvent:                          return "TypeDeviceEvent";
		case spv::OpTypeReserveId:                            return "TypeReserveId";
		case spv::OpTypeQueue:                                return "TypeQueue";
		case spv::OpTypePipe:                                 return "TypePipe";
		case spv::OpConstantTrue:                             return "ConstantTrue";
		case spv::OpConstantFalse:                            return "ConstantFalse";
		case spv::OpConstant:                                 return "Constant";
		case spv::OpConstantComposite:                        return "ConstantComposite";
		case spv::OpConstantSampler:                          return "ConstantSampler";
		case spv::OpConstantNullPointer:                      return "ConstantNullPointer";
		case spv::OpConstantNullObject:                       return "ConstantNullObject";
		case spv::OpSpecConstantTrue:                         return "SpecConstantTrue";
		case spv::OpSpecConstantFalse:                        return "SpecConstantFalse";
		case spv::OpSpecConstant:                             return "SpecConstant";
		case spv::OpSpecConstantComposite:                    return "SpecConstantComposite";
		case spv::OpVariable:                                 return "Variable";
		case spv::OpVariableArray:                            return "VariableArray";
		case spv::OpFunction:                                 return "Function";
		case spv::OpFunctionParameter:                        return "FunctionParameter";
		case spv::OpFunctionEnd:                              return "FunctionEnd";
		case spv::OpFunctionCall:                             return "FunctionCall";
		case spv::OpExtInst:                                  return "ExtInst";
		case spv::OpUndef:                                    return "Undef";
		case spv::OpLoad:                                     return "Load";
		case spv::OpStore:                                    return "Store";
		case spv::OpPhi:                                      return "Phi";
		case spv::OpDecorationGroup:                          return "DecorationGroup";
		case spv::OpDecorate:                                 return "Decorate";
		case spv::OpMemberDecorate:                           return "MemberDecorate";
		case spv::OpGroupDecorate:                            return "GroupDecorate";
		case spv::OpGroupMemberDecorate:                      return "GroupMemberDecorate";
		case spv::OpName:                                     return "Name";
		case spv::OpMemberName:                               return "MemberName";
		case spv::OpString:                                   return "String";
		case spv::OpLine:                                     return "Line";
		case spv::OpVectorExtractDynamic:                     return "VectorExtractDynamic";
		case spv::OpVectorInsertDynamic:                      return "VectorInsertDynamic";
		case spv::OpVectorShuffle:                            return "VectorShuffle";
		case spv::OpCompositeConstruct:                       return "CompositeConstruct";
		case spv::OpCompositeExtract:                         return "CompositeExtract";
		case spv::OpCompositeInsert:                          return "CompositeInsert";
		case spv::OpCopyObject:                               return "CopyObject";
		case spv::OpCopyMemory:                               return "CopyMemory";
		case spv::OpCopyMemorySized:                          return "CopyMemorySized";
		case spv::OpSampler:                                  return "Sampler";
		case spv::OpTextureSample:                            return "TextureSample";
		case spv::OpTextureSampleDref:                        return "TextureSampleDref";
		case spv::OpTextureSampleLod:                         return "TextureSampleLod";
		case spv::OpTextureSampleProj:                        return "TextureSampleProj";
		case spv::OpTextureSampleGrad:                        return "TextureSampleGrad";
		case spv::OpTextureSampleOffset:                      return "TextureSampleOffset";
		case spv::OpTextureSampleProjLod:                     return "TextureSampleProjLod";
		case spv::OpTextureSampleProjGrad:                    return "TextureSampleProjGrad";
		case spv::OpTextureSampleLodOffset:                   return "TextureSampleLodOffset";
		case spv::OpTextureSampleProjOffset:                  return "TextureSampleProjOffset";
		case spv::OpTextureSampleGradOffset:                  return "TextureSampleGradOffset";
		case spv::OpTextureSampleProjLodOffset:               return "TextureSampleProjLodOffset";
		case spv::OpTextureSampleProjGradOffset:              return "TextureSampleProjGradOffset";
		case spv::OpTextureFetchTexelLod:                     return "TextureFetchTexelLod";
		case spv::OpTextureFetchTexelOffset:                  return "TextureFetchTexelOffset";
		case spv::OpTextureFetchSample:                       return "TextureFetchSample";
		case spv::OpTextureFetchTexel:                        return "TextureFetchTexel";
		case spv::OpTextureGather:                            return "TextureGather";
		case spv::OpTextureGatherOffset:                      return "TextureGatherOffset";
		case spv::OpTextureGatherOffsets:                     return "TextureGatherOffsets";
		case spv::OpTextureQuerySizeLod:                      return "TextureQuerySizeLod";
		case spv::OpTextureQuerySize:                         return "TextureQuerySize";
		case spv::OpTextureQueryLod:                          return "TextureQueryLod";
		case spv::OpTextureQueryLevels:                       return "TextureQueryLevels";
		case spv::OpTextureQuerySamples:                      return "TextureQuerySamples";
		case spv::OpAccessChain:                              return "AccessChain";
		case spv::OpInBoundsAccessChain:                      return "InBoundsAccessChain";
		case spv::OpSNegate:                                  return "SNegate";
		case spv::OpFNegate:                                  return "FNegate";
		case spv::OpNot:                                      return "Not";
		case spv::OpAny:                                      return "Any";
		case spv::OpAll:                                      return "All";
		case spv::OpConvertFToU:                              return "ConvertFToU";
		case spv::OpConvertFToS:                              return "ConvertFToS";
		case spv::OpConvertSToF:                              return "ConvertSToF";
		case spv::OpConvertUToF:                              return "ConvertUToF";
		case spv::OpUConvert:                                 return "UConvert";
		case spv::OpSConvert:                                 return "SConvert";
		case spv::OpFConvert:                                 return "FConvert";
		case spv::OpConvertPtrToU:                            return "ConvertPtrToU";
		case spv::OpConvertUToPtr:                            return "ConvertUToPtr";
		case spv::OpPtrCastToGeneric:                         return "PtrCastToGeneric";
		case spv::OpGenericCastToPtr:                         return "GenericCastToPtr";
		case spv::OpBitcast:                                  return "Bitcast";
		case spv::OpTranspose:                                return "Transpose";
		case spv::OpIsNan:                                    return "IsNan";
		case spv::OpIsInf:                                    return "IsInf";
		case spv::OpIsFinite:                                 return "IsFinite";
		case spv::OpIsNormal:                                 return "IsNormal";
		case spv::OpSignBitSet:                               return "SignBitSet";
		case spv::OpLessOrGreater:                            return "LessOrGreater";
		case spv::OpOrdered:                                  return "Ordered";
		case spv::OpUnordered:                                return "Unordered";
		case spv::OpArrayLength:                              return "ArrayLength";
		case spv::OpIAdd:                                     return "IAdd";
		case spv::OpFAdd:                                     return "FAdd";
		case spv::OpISub:                                     return "ISub";
		case spv::OpFSub:                                     return "FSub";
		case spv::OpIMul:                                     return "IMul";
		case spv::OpFMul:                                     return "FMul";
		case spv::OpUDiv:                                     return "UDiv";
		case spv::OpSDiv:                                     return "SDiv";
		case spv::OpFDiv:                                     return "FDiv";
		case spv::OpUMod:                                     return "UMod";
		case spv::OpSRem:                                     return "SRem";
		case spv::OpSMod:                                     return "SMod";
		case spv::OpFRem:                                     return "FRem";
		case spv::OpFMod:                                     return "FMod";
		case spv::OpVectorTimesScalar:                        return "VectorTimesScalar";
		case spv::OpMatrixTimesScalar:                        return "MatrixTimesScalar";
		case spv::OpVectorTimesMatrix:                        return "VectorTimesMatrix";
		case spv::OpMatrixTimesVector:                        return "MatrixTimesVector";
		case spv::OpMatrixTimesMatrix:                        return "MatrixTimesMatrix";
		case spv::OpOuterProduct:                             return "OuterProduct";
		case spv::OpDot:                                      return "Dot";
		case spv::OpShiftRightLogical:                        return "ShiftRightLogical";
		case spv::OpShiftRightArithmetic:                     return "ShiftRightArithmetic";
		case spv::OpShiftLeftLogical:                         return "ShiftLeftLogical";
		case spv::OpLogicalOr:                                return "LogicalOr";
		case spv::OpLogicalXor:                               return "LogicalXor";
		case spv::OpLogicalAnd:                               return "LogicalAnd";
		case spv::OpBitwiseOr:                                return "BitwiseOr";
		case spv::OpBitwiseXor:                               return "BitwiseXor";
		case spv::OpBitwiseAnd:                               return "BitwiseAnd";
		case spv::OpSelect:                                   return "Select";
		case spv::OpIEqual:                                   return "IEqual";
		case spv::OpFOrdEqual:                                return "FOrdEqual";
		case spv::OpFUnordEqual:                              return "FUnordEqual";
		case spv::OpINotEqual:                                return "INotEqual";
		case spv::OpFOrdNotEqual:                             return "FOrdNotEqual";
		case spv::OpFUnordNotEqual:                           return "FUnordNotEqual";
		case spv::OpULessThan:                                return "ULessThan";
		case spv::OpSLessThan:                                return "SLessThan";
		case spv::OpFOrdLessThan:                             return "FOrdLessThan";
		case spv::OpFUnordLessThan:                           return "FUnordLessThan";
		case spv::OpUGreaterThan:                             return "UGreaterThan";
		case spv::OpSGreaterThan:                             return "SGreaterThan";
		case spv::OpFOrdGreaterThan:                          return "FOrdGreaterThan";
		case spv::OpFUnordGreaterThan:                        return "FUnordGreaterThan";
		case spv::OpULessThanEqual:                           return "ULessThanEqual";
		case spv::OpSLessThanEqual:                           return "SLessThanEqual";
		case spv::OpFOrdLessThanEqual:                        return "FOrdLessThanEqual";
		case spv::OpFUnordLessThanEqual:                      return "FUnordLessThanEqual";
		case spv::OpUGreaterThanEqual:                        return "UGreaterThanEqual";
		case spv::OpSGreaterThanEqual:                        return "SGreaterThanEqual";
		case spv::OpFOrdGreaterThanEqual:                     return "FOrdGreaterThanEqual";
		case spv::OpFUnordGreaterThanEqual:                   return "FUnordGreaterThanEqual";
		case spv::OpDPdx:                                     return "DPdx";
		case spv::OpDPdy:                                     return "DPdy";
		case spv::OpFwidth:                                   return "Fwidth";
		case spv::OpDPdxFine:                                 return "DPdxFine";
		case spv::OpDPdyFine:                                 return "DPdyFine";
		case spv::OpFwidthFine:                               return "FwidthFine";
		case spv::OpDPdxCoarse:                               return "DPdxCoarse";
		case spv::OpDPdyCoarse:                               return "DPdyCoarse";
		case spv::OpFwidthCoarse:                             return "FwidthCoarse";
		case spv::OpEmitVertex:                               return "EmitVertex";
		case spv::OpEndPrimitive:                             return "EndPrimitive";
		case spv::OpEmitStreamVertex:                         return "EmitStreamVertex";
		case spv::OpEndStreamPrimitive:                       return "EndStreamPrimitive";
		case spv::OpControlBarrier:                           return "ControlBarrier";
		case spv::OpMemoryBarrier:                            return "MemoryBarrier";
		case spv::OpImagePointer:                             return "ImagePointer";
		case spv::OpAtomicInit:                               return "AtomicInit";
		case spv::OpAtomicLoad:                               return "AtomicLoad";
		case spv::OpAtomicStore:                              return "AtomicStore";
		case spv::OpAtomicExchange:                           return "AtomicExchange";
		case spv::OpAtomicCompareExchange:                    return "AtomicCompareExchange";
		case spv::OpAtomicCompareExchangeWeak:                return "AtomicCompareExchangeWeak";
		case spv::OpAtomicIIncrement:                         return "AtomicIIncrement";
		case spv::OpAtomicIDecrement:                         return "AtomicIDecrement";
		case spv::OpAtomicIAdd:                               return "AtomicIAdd";
		case spv::OpAtomicISub:                               return "AtomicISub";
		case spv::OpAtomicUMin:                               return "AtomicUMin";
		case spv::OpAtomicUMax:                               return "AtomicUMax";
		case spv::OpAtomicAnd:                                return "AtomicAnd";
		case spv::OpAtomicOr:                                 return "AtomicOr";
		case spv::OpAtomicXor:                                return "AtomicXor";
		case spv::OpLoopMerge:                                return "LoopMerge";
		case spv::OpSelectionMerge:                           return "SelectionMerge";
		case spv::OpLabel:                                    return "Label";
		case spv::OpBranch:                                   return "Branch";
		case spv::OpBranchConditional:                        return "BranchConditional";
		case spv::OpSwitch:                                   return "Switch";
		case spv::OpKill:                                     return "Kill";
		case spv::OpReturn:                                   return "Return";
		case spv::OpReturnValue:                              return "ReturnValue";
		case spv::OpUnreachable:                              return "Unreachable";
		case spv::OpLifetimeStart:                            return "LifetimeStart";
		case spv::OpLifetimeStop:                             return "LifetimeStop";
		case spv::OpCompileFlag:                              return "CompileFlag";
		case spv::OpAsyncGroupCopy:                           return "AsyncGroupCopy";
		case spv::OpWaitGroupEvents:                          return "WaitGroupEvents";
		case spv::OpGroupAll:                                 return "GroupAll";
		case spv::OpGroupAny:                                 return "GroupAny";
		case spv::OpGroupBroadcast:                           return "GroupBroadcast";
		case spv::OpGroupIAdd:                                return "GroupIAdd";
		case spv::OpGroupFAdd:                                return "GroupFAdd";
		case spv::OpGroupFMin:                                return "GroupFMin";
		case spv::OpGroupUMin:                                return "GroupUMin";
		case spv::OpGroupSMin:                                return "GroupSMin";
		case spv::OpGroupFMax:                                return "GroupFMax";
		case spv::OpGroupUMax:                                return "GroupUMax";
		case spv::OpGroupSMax:                                return "GroupSMax";
		case spv::OpGenericCastToPtrExplicit:                 return "GenericCastToPtrExplicit";
		case spv::OpGenericPtrMemSemantics:                   return "GenericPtrMemSemantics";
		case spv::OpReadPipe:                                 return "ReadPipe";
		case spv::OpWritePipe:                                return "WritePipe";
		case spv::OpReservedReadPipe:                         return "ReservedReadPipe";
		case spv::OpReservedWritePipe:                        return "ReservedWritePipe";
		case spv::OpReserveReadPipePackets:                   return "ReserveReadPipePackets";
		case spv::OpReserveWritePipePackets:                  return "ReserveWritePipePackets";
		case spv::OpCommitReadPipe:                           return "CommitReadPipe";
		case spv::OpCommitWritePipe:                          return "CommitWritePipe";
		case spv::OpIsValidReserveId:                         return "IsValidReserveId";
		case spv::OpGetNumPipePackets:                        return "GetNumPipePackets";
		case spv::OpGetMaxPipePackets:                        return "GetMaxPipePackets";
		case spv::OpGroupReserveReadPipePackets:              return "GroupReserveReadPipePackets";
		case spv::OpGroupReserveWritePipePackets:             return "GroupReserveWritePipePackets";
		case spv::OpGroupCommitReadPipe:                      return "GroupCommitReadPipe";
		case spv::OpGroupCommitWritePipe:                     return "GroupCommitWritePipe";
		case spv::OpEnqueueMarker:                            return "EnqueueMarker";
		case spv::OpEnqueueKernel:                            return "EnqueueKernel";
		case spv::OpGetKernelNDrangeSubGroupCount:            return "GetKernelNDrangeSubGroupCount";
		case spv::OpGetKernelNDrangeMaxSubGroupSize:          return "GetKernelNDrangeMaxSubGroupSize";
		case spv::OpGetKernelWorkGroupSize:                   return "GetKernelWorkGroupSize";
		case spv::OpGetKernelPreferredWorkGroupSizeMultiple:  return "GetKernelPreferredWorkGroupSizeMultiple";
		case spv::OpRetainEvent:                              return "RetainEvent";
		case spv::OpReleaseEvent:                             return "ReleaseEvent";
		case spv::OpCreateUserEvent:                          return "CreateUserEvent";
		case spv::OpIsValidEvent:                             return "IsValidEvent";
		case spv::OpSetUserEventStatus:                       return "SetUserEventStatus";
		case spv::OpCaptureEventProfilingInfo:                return "CaptureEventProfilingInfo";
		case spv::OpGetDefaultQueue:                          return "GetDefaultQueue";
		case spv::OpBuildNDRange:                             return "BuildNDRange";
		case spv::OpSatConvertSToU:                           return "SatConvertSToU";
		case spv::OpSatConvertUToS:                           return "SatConvertUToS";
		case spv::OpAtomicIMin:                               return "AtomicIMin";
		case spv::OpAtomicIMax:                               return "AtomicIMax";
		default: break;
	}
	
	return StringFormat::Fmt("Unrecognised{%u}", (uint32_t)el);
}

template<>
string ToStrHelper<false, spv::SourceLanguage>::Get(const spv::SourceLanguage &el)
{
	switch(el)
	{
		case spv::SourceLanguageUnknown: return "Unknown";
		case spv::SourceLanguageESSL:    return "ESSL";
		case spv::SourceLanguageGLSL:    return "GLSL";
		case spv::SourceLanguageOpenCL:  return "OpenCL";
		default: break;
	}
	
	return "Unrecognised";
}

template<>
string ToStrHelper<false, spv::AddressingModel>::Get(const spv::AddressingModel &el)
{
	switch(el)
	{
		case spv::AddressingModelLogical:    return "Logical";
		case spv::AddressingModelPhysical32: return "Physical (32-bit)";
		case spv::AddressingModelPhysical64: return "Physical (64-bit)";
		default: break;
	}
	
	return "Unrecognised";
}

template<>
string ToStrHelper<false, spv::MemoryModel>::Get(const spv::MemoryModel &el)
{
	switch(el)
	{
		case spv::MemoryModelSimple:   return "Simple";
		case spv::MemoryModelGLSL450:  return "GLSL450";
		case spv::MemoryModelOpenCL12: return "OpenCL12";
		case spv::MemoryModelOpenCL20: return "OpenCL20";
		case spv::MemoryModelOpenCL21: return "OpenCL21";
		default: break;
	}
	
	return "Unrecognised";
}

template<>
string ToStrHelper<false, spv::ExecutionModel>::Get(const spv::ExecutionModel &el)
{
	switch(el)
	{
		case spv::ExecutionModelVertex:    return "Vertex Shader";
		case spv::ExecutionModelTessellationControl: return "Tess. Control Shader";
		case spv::ExecutionModelTessellationEvaluation: return "Tess. Eval Shader";
		case spv::ExecutionModelGeometry:  return "Geometry Shader";
		case spv::ExecutionModelFragment:  return "Fragment Shader";
		case spv::ExecutionModelGLCompute: return "Compute Shader";
		case spv::ExecutionModelKernel:    return "Kernel";
		default: break;
	}
	
	return "Unrecognised";
}

template<>
string ToStrHelper<false, spv::Decoration>::Get(const spv::Decoration &el)
{
	switch(el)
	{
    case spv::DecorationPrecisionLow:         return "PrecisionLow";
    case spv::DecorationPrecisionMedium:      return "PrecisionMedium";
    case spv::DecorationPrecisionHigh:        return "PrecisionHigh";
    case spv::DecorationBlock:                return "Block";
    case spv::DecorationBufferBlock:          return "BufferBlock";
    case spv::DecorationRowMajor:             return "RowMajor";
    case spv::DecorationColMajor:             return "ColMajor";
    case spv::DecorationGLSLShared:           return "GLSLShared";
    case spv::DecorationGLSLStd140:           return "GLSLStd140";
    case spv::DecorationGLSLStd430:           return "GLSLStd430";
    case spv::DecorationGLSLPacked:           return "GLSLPacked";
    case spv::DecorationSmooth:               return "Smooth";
    case spv::DecorationNoperspective:        return "Noperspective";
    case spv::DecorationFlat:                 return "Flat";
    case spv::DecorationPatch:                return "Patch";
    case spv::DecorationCentroid:             return "Centroid";
    case spv::DecorationSample:               return "Sample";
    case spv::DecorationInvariant:            return "Invariant";
    case spv::DecorationRestrict:             return "Restrict";
    case spv::DecorationAliased:              return "Aliased";
    case spv::DecorationVolatile:             return "Volatile";
    case spv::DecorationConstant:             return "Constant";
    case spv::DecorationCoherent:             return "Coherent";
    case spv::DecorationNonwritable:          return "Nonwritable";
    case spv::DecorationNonreadable:          return "Nonreadable";
    case spv::DecorationUniform:              return "Uniform";
    case spv::DecorationNoStaticUse:          return "NoStaticUse";
    case spv::DecorationCPacked:              return "CPacked";
    case spv::DecorationSaturatedConversion:  return "SaturatedConversion";
    case spv::DecorationStream:               return "Stream";
    case spv::DecorationLocation:             return "Location";
    case spv::DecorationComponent:            return "Component";
    case spv::DecorationIndex:                return "Index";
    case spv::DecorationBinding:              return "Binding";
    case spv::DecorationDescriptorSet:        return "DescriptorSet";
    case spv::DecorationOffset:               return "Offset";
    case spv::DecorationAlignment:            return "Alignment";
    case spv::DecorationXfbBuffer:            return "XfbBuffer";
    case spv::DecorationStride:               return "Stride";
    case spv::DecorationBuiltIn:              return "BuiltIn";
    case spv::DecorationFuncParamAttr:        return "FuncParamAttr";
    case spv::DecorationFPRoundingMode:       return "FPRoundingMode";
    case spv::DecorationFPFastMathMode:       return "FPFastMathMode";
    case spv::DecorationLinkageAttributes:    return "LinkageAttributes";
    case spv::DecorationSpecId:               return "SpecId";
		default: break;
	}
	
	return "Unrecognised";
}

template<>
string ToStrHelper<false, spv::StorageClass>::Get(const spv::StorageClass &el)
{
	switch(el)
	{
    case spv::StorageClassUniformConstant:    return "UniformConstant";
    case spv::StorageClassInput:              return "Input";
    case spv::StorageClassUniform:            return "Uniform";
    case spv::StorageClassOutput:             return "Output";
    case spv::StorageClassWorkgroupLocal:     return "WorkgroupLocal";
    case spv::StorageClassWorkgroupGlobal:    return "WorkgroupGlobal";
    case spv::StorageClassPrivateGlobal:      return "PrivateGlobal";
    case spv::StorageClassFunction:           return "Function";
    case spv::StorageClassGeneric:            return "Generic";
    case spv::StorageClassPrivate:            return "Private";
    case spv::StorageClassAtomicCounter:      return "AtomicCounter";
		default: break;
	}
	
	return "Unrecognised";
}

template<>
string ToStrHelper<false, spv::FunctionControlMask>::Get(const spv::FunctionControlMask &el)
{
	string ret;

	if(el & spv::FunctionControlInlineMask)     ret += ", Inline";
	if(el & spv::FunctionControlDontInlineMask) ret += ", DontInline";
	if(el & spv::FunctionControlPureMask)       ret += ", Pure";
	if(el & spv::FunctionControlConstMask)      ret += ", Const";
	
	if(!ret.empty())
		ret = ret.substr(2);

	return ret;
}

template<>
string ToStrHelper<false, spv::SelectionControlMask>::Get(const spv::SelectionControlMask &el)
{
	string ret;

	if(el & spv::SelectionControlFlattenMask)     ret += ", Flatten";
	if(el & spv::SelectionControlDontFlattenMask) ret += ", DontFlatten";
	
	if(!ret.empty())
		ret = ret.substr(2);

	return ret;
}

template<>
string ToStrHelper<false, spv::LoopControlMask>::Get(const spv::LoopControlMask &el)
{
	string ret;

	if(el & spv::LoopControlUnrollMask)     ret += ", Unroll";
	if(el & spv::LoopControlDontUnrollMask) ret += ", DontUnroll";
	
	if(!ret.empty())
		ret = ret.substr(2);

	return ret;
}

template<>
string ToStrHelper<false, spv::MemoryAccessMask>::Get(const spv::MemoryAccessMask &el)
{
	string ret;
	
	if(el & spv::MemoryAccessVolatileMask)     ret += ", Volatile";
	if(el & spv::MemoryAccessAlignedMask) ret += ", Aligned";
	
	if(!ret.empty())
		ret = ret.substr(2);

	return ret;
}
