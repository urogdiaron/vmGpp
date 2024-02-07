#if 0
#include "Base/ucBase.h"
#include "Core/ucCore.h"
#include "ECS/ecs2/ucECS2.h"
#include "catch/catch.hpp"

using namespace unicorn;

static void BoilerInit()
{
	using namespace ecs2;

	Manager::Init();
}

static void BoilerShutdown()
{
	using namespace ecs2;
	Manager::Shutdown();
}

namespace
{
	struct BoilerScope
	{
		BoilerScope() { BoilerInit(); }
		~BoilerScope() { BoilerShutdown(); }
	};
}

namespace gpp3_vm
{
	enum class DataType
	{
		Error, Void, Float, Bool, Int, Count
	};

	template<typename TData>
	DataType getDataType() { return Error; }

	template<> DataType getDataType<float>() { return DataType::Float; }
	template<> DataType getDataType<bool>() { return DataType::Bool; }
	template<> DataType getDataType<int>() { return DataType::Int; }

	template<DataType TDataType>
	struct TypesByDataType
	{
		using Type = void;
	};

	template<> struct TypesByDataType<DataType::Float> { using Type = float; };
	template<> struct TypesByDataType<DataType::Bool> { using Type = bool; };
	template<> struct TypesByDataType<DataType::Int> { using Type = int; };

	uint32_t nextVariableId = 1;
	struct Variable;
	std::unordered_map<uint32_t, Variable*> variables;

	struct Variable
	{
		float currentValue = 0.0f;
		uint32_t id;
		Variable() : id(nextVariableId++) { variables[id] = this; }
		~Variable() { variables.erase(id); }
	};

	enum OpCode : uint32_t
	{
		EXIT,
		PUSH,	// push 32 bit
		POP,
		LOAD,
		LOAD_PARAM,
		STORE,
		JUMP,
		JUMP_IF_NOT,
		JUMP_IF_EQUALS,
		CALL,
		RETURN,
		CONVERT,
		COUNT
	};

	union InstructionData
	{
		uint64_t data_uint64_t;
		float data_f[2];
		int32_t data_i[2];
	};

	struct Instruction
	{
		uint32_t opCode;
		InstructionData data;

		Instruction(uint32_t opCode) : opCode(opCode), data{} {}

		Instruction(uint32_t opCode, uint64_t data_uint64_t) 
			: opCode(opCode)
		{
			data.data_uint64_t = data_uint64_t;
		}

		Instruction(uint32_t opCode, float data_f0, float data_f1 = 0.0f)
			: opCode(opCode)
		{
			data.data_f[0] = data_f0;
			data.data_f[1] = data_f1;
		}

		Instruction(uint32_t opCode, int32_t data_i0, int32_t data_i1 = 0)
			: opCode(opCode)
		{
			data.data_i[0] = data_i0;
			data.data_i[1] = data_i1;
		}

		Instruction(uint32_t opCode, float data_f0, int32_t data_i1)
			: opCode(opCode)
		{
			data.data_f[0] = data_f0;
			data.data_f[1] = (float&)data_i1;
		}
	};

	struct Code
	{
		std::vector<Instruction> instructions;
	};

	struct VM;
	struct OperationRegistry
	{
		std::vector<void(*)(VM&)> operations;
		static inline std::unique_ptr<OperationRegistry> instance = {};

		static OperationRegistry& getInstance()
		{
			if (instance)
				return *instance.get();

			instance = std::make_unique<OperationRegistry>();
			return *instance.get();
		}

		OperationRegistry();

		uint32_t registerOperation(void(*f)(VM&))
		{
			operations.push_back(f);
			return operations.size() - 1;
		}

		void exec(VM& vm, uint32_t opCode)
		{
			operations[opCode](vm);
		}
	};

	struct VM
	{
		std::vector<uint8_t> stack;
		Instruction* currentInstruction;
		size_t stackBase = 0;

		VM() 
		{
			stack.reserve(1 << 20); // reserve 1 MB of stack
			push(nullptr);
			push(stackBase);
			stackBase = stack.size();
		}

		void run(Code& code)
		{
			OperationRegistry& operationRegistry = OperationRegistry::getInstance();
			currentInstruction = code.instructions.data();
			while (currentInstruction && currentInstruction->opCode)
			{
				operationRegistry.exec(*this, currentInstruction->opCode);
			}
		}

		template<typename TData>
		void push(const TData& data)
		{
			auto currentSize = stack.size();
			stack.resize(currentSize + sizeof(TData));
			memcpy(stack.data() + currentSize, &data, sizeof(TData));
		}

		template<typename TData>
		TData pop()
		{
			TData ret;
			memcpy(&ret, stack.data() + stack.size() - sizeof(TData), sizeof(TData));
			memset(&*(stack.end() - sizeof(TData)), 0xcdcdcdcd, sizeof(TData));
			stack.erase(stack.end() - sizeof(TData), stack.end());
			return ret;
		}

		void push(void* data, size_t size)
		{
			auto currentSize = stack.size();
			stack.resize(currentSize + size);
			memcpy(stack.data() + currentSize, data, size);
		}

		void pop(size_t size)
		{
			memset(&*(stack.end() - size), 0xcdcdcdcd, size);
			stack.erase(stack.end() - size, stack.end());
		}
	};

	void push(VM& vm)
	{
		vm.push((uint32_t&)vm.currentInstruction->data.data_f[0]);
		vm.currentInstruction++;
	}

	void pop(VM& vm)
	{
		vm.pop(vm.currentInstruction->data.data_uint64_t);
		vm.currentInstruction++;
	}

	void load(VM& vm)
	{
		int32_t relativeStackPos = vm.currentInstruction->data.data_i[0];
		size_t address = vm.stackBase + relativeStackPos;
		vm.push(&vm.stack[address], sizeof(float));
		vm.currentInstruction++;
	}

	void loadParam(VM& vm)
	{
		int32_t paramOffset = vm.currentInstruction->data.data_i[0];
		// skip the return address, skip the stackBase, then skip over the params
		// accounting for the param sizes is still the caller's job
		int32_t relativeStackPosBackwards = sizeof(Instruction*) + sizeof(size_t) + paramOffset;
		size_t address = vm.stackBase - relativeStackPosBackwards;
		vm.push(&vm.stack[address], sizeof(float));
		vm.currentInstruction++;
	}

	void store(VM& vm)
	{
		int32_t relativeStackPos = vm.currentInstruction->data.data_i[0];
		size_t address = vm.stackBase + relativeStackPos;
		float valueOnStack = vm.pop<float>();
		memcpy(&vm.stack[address], &valueOnStack, sizeof(float));
		vm.currentInstruction++;
	}

	void jump(VM& vm)
	{
		int32_t jumpSize = vm.currentInstruction->data.data_i[0];
		vm.currentInstruction += jumpSize;
	}

	void jumpIfEquals(VM& vm)
	{
		float valueOnStack = vm.pop<float>();
		float valueToCompare = vm.currentInstruction->data.data_f[0];
		int32_t jumpSize = (int32_t&)vm.currentInstruction->data.data_f[1];
		if (valueToCompare == valueOnStack)
		{
			vm.currentInstruction += jumpSize;
		}
		else
		{
			vm.currentInstruction++;
		}
	}

	void jumpIfNot(VM& vm)
	{
		bool condition = vm.pop<bool>();
		int32_t jumpSize = vm.currentInstruction->data.data_i[0];
		if (!condition)
		{
			vm.currentInstruction += jumpSize;
		}
		else
		{
			vm.currentInstruction++;
		}
	}

	void call(VM& vm)
	{
		vm.push(vm.currentInstruction + 1);
		vm.push(vm.stackBase);
		vm.stackBase = vm.stack.size();
		vm.currentInstruction += vm.currentInstruction->data.data_i[0];
	}

	void ret(VM& vm)
	{
		vm.stackBase = vm.pop<size_t>();
		vm.currentInstruction = vm.pop<Instruction*>();
	}

	void convert(VM& vm)
	{
		DataType from = (DataType&)vm.currentInstruction->data.data_i[0];
		DataType to = (DataType&)vm.currentInstruction->data.data_i[1];
		vm.currentInstruction++;

		if (from == to)
		{
			return;
		}

		switch (from)
		{
		case DataType::Float:
			{
				auto v = vm.pop<float>();
				switch (to)
				{
				case DataType::Bool:
					{
						vm.push(static_cast<bool>(v));
					}
					break;
				case DataType::Int:
					{
						vm.push(static_cast<int>(v));
					}
					break;
				}
			}
			break;
		case DataType::Bool:
			{
				bool v = vm.pop<bool>();
				switch (to)
				{
				case DataType::Float:
					{
						vm.push(static_cast<float>(v));
					}
					break;
				case DataType::Int:
					{
						vm.push(static_cast<int>(v));
					}
					break;
				}
			}
			break;
		}
	}

	OperationRegistry::OperationRegistry()
	{
		operations = 
		{
			nullptr,
			gpp3_vm::push,
			gpp3_vm::pop,
			gpp3_vm::load,
			gpp3_vm::loadParam,
			gpp3_vm::store,
			gpp3_vm::jump,
			gpp3_vm::jumpIfNot,
			gpp3_vm::jumpIfEquals,
			gpp3_vm::call,
			gpp3_vm::ret,
			gpp3_vm::convert,
		};
	}

	struct FunctionId
	{
		uint32_t propId;
		uint8_t actionId;

		bool operator<(const FunctionId& rhs) const
		{
			return std::make_tuple(propId, actionId) < std::make_tuple(rhs.propId, actionId);
		}

		bool operator==(const FunctionId& rhs) const
		{
			return std::make_tuple(propId, actionId) == std::make_tuple(rhs.propId, actionId);
		}
	};

	struct CompilationContext
	{
		struct Function
		{
			struct CallSite
			{
				FunctionId callingFunctionId;
				size_t callInstructionIndex;
			};

			std::vector<Instruction> instructions;
			std::vector<CallSite> callSites;
			size_t startingAddress = 0;
		};

		CompilationContext()
		{
			functions[{0, 0}] = Function();
			functionStack.push_back({0, 0});
		}

		bool callToFunction(FunctionId functionId)
		{
			bool functionMustBeCompiled = true;
			auto itFunc = functions.find(functionId);
			if (itFunc == functions.end())
			{
				functions[functionId] = Function();
			}
			else
			{
				functionMustBeCompiled = false;
			}

			// Add a call instr, the jump size is unknown so we remember the instruction in the function so we can fill it in at the end
			FunctionId currentFuncId = functionStack.back();
			Function* currentFunc = getCurrentFunction();
			size_t callIndex = currentFunc->instructions.size();
			currentFunc->instructions.push_back({ OpCode::CALL, 0 });

			Function& newFunction = functions[functionId];
			functionStack.push_back(functionId);
			newFunction.callSites.push_back({ currentFuncId, callIndex });

			return functionMustBeCompiled;
		}

		Function* getCurrentFunction()
		{
			FunctionId id = functionStack.back();
			return &functions[id];
		}

		void returnFromFunction()
		{
			Function* currentFunc = getCurrentFunction();
			currentFunc->instructions.push_back({ OpCode::RETURN });
			functionStack.pop_back();
		}

		void add(const Instruction& inst)
		{
			Function* currentFunc = getCurrentFunction();
			currentFunc->instructions.push_back(inst);
		}

		size_t getNextInstructionIndex()
		{
			Function* currentFunc = getCurrentFunction();
			return currentFunc->instructions.size();
		}

		Instruction& getInstruction(size_t index)
		{
			Function* currentFunc = getCurrentFunction();
			return currentFunc->instructions[index];
		}

		// For removing final RETURN calls for inlining
		void popLastInstruction()
		{
			Function* currentFunc = getCurrentFunction();
			return currentFunc->instructions.pop_back();
		}

		Code finalizeCode()
		{
			Code retCode;
			std::vector<Instruction>& ret = retCode.instructions;

			// In one pass we figure out the starting address for every function
			size_t address = 0;
			for (auto& itFunc : functions)
			{
				itFunc.second.startingAddress = address;
				address += itFunc.second.instructions.size();
			}

			// Patch up all calling sites
			for (auto& itFunc : functions)
			{
				for (auto& callSite : itFunc.second.callSites)
				{
					Function& callingFunction = functions[callSite.callingFunctionId];
					size_t addressOfCall = callSite.callInstructionIndex + callingFunction.startingAddress;
					callingFunction.instructions[callSite.callInstructionIndex].data.data_i[0] = itFunc.second.startingAddress - addressOfCall;
				}
			}

			// Now copy everything together
			for (auto& itFunc : functions)
			{
				ret.insert(ret.end(), itFunc.second.instructions.begin(), itFunc.second.instructions.end());
			}
			ret.push_back({ OpCode::RETURN });

			return retCode;
		}

	private:
		std::map<FunctionId, Function> functions;
		std::vector<FunctionId> functionStack;
		uint32_t nextFunctionId = 1;
		uint32_t currentFunctionId = 1;
	};

	uint32_t nextPropId = 1;

	struct Prop;
	std::map<uint32_t, Prop*> props;

	struct Prop
	{
		Prop() : id(nextPropId++) { props[id] = this; }
		~Prop() { props.erase(id); }

		uint32_t id;
		virtual DataType compile(CompilationContext& context, uint8_t outputIndex) { return DataType::Error; }
	};

	template<typename TData>
	struct Input
	{
		Prop* sourceProp = nullptr;
		uint8_t sourcePortIndex = 0;

		TData defaultValue;

		DataType compile(CompilationContext& context) const
		{
			if (sourceProp)
			{
				DataType dataType = sourceProp->compile(context, sourcePortIndex);
				switch (dataType)
				{
					case DataType::Void:
					{
						// Do nothing
					}
					break;
					case DataType::Error:
					{
						return DataType::Error;
					}
					break;
					default:
					{
						if (dataType != getDataType<TData>())
						{
							context.add({ OpCode::CONVERT, (int)dataType, (int)getDataType<TData>() });
						}
					}
					break;
				}
			}
			else
			{
				context.add({ OpCode::PUSH, defaultValue });
			}
			return getDataType<TData>();
		}
	};

	template<void (*func)(VM&)>
	struct Operation
	{
		Operation()
			: opCode(OperationRegistry::getInstance().registerOperation(func))
		{}

		operator uint32_t() const { return opCode; }
		uint32_t opCode;

	};

	struct AddProp : Prop
	{
		Input<float> a;
		Input<float> b;

		static void add(VM& vm)
		{
			float v1 = vm.pop<float>();
			float v2 = vm.pop<float>();
			vm.push(v1 + v2);
			vm.currentInstruction++;
		}

		static inline Operation<add> op;

		DataType compile(CompilationContext& context, uint8_t outputPortIndex) override
		{
			if (outputPortIndex != 0)
				return DataType::Error;

			a.compile(context);
			b.compile(context);
			context.add({ op.opCode });

			return DataType::Float;
		}
	};

	struct PrintProp : Prop
	{
		Input<float> a;

		static void print(VM& vm)
		{
			float a = vm.pop<float>();
			printf("Print prop: %.02f\n", a);
			vm.currentInstruction++;
		}

		static inline Operation<print> op;

		DataType compile(CompilationContext& context, uint8_t outputPortIndex) override
		{
			if (outputPortIndex != 0)
				return DataType::Error;

			a.compile(context);
			context.add({ op.opCode });

			return DataType::Void;
		}
	};

	struct TestProp : Prop
	{
		Input<float> a;
		Input<float> b;

		static void test(VM& vm)
		{
			float a = vm.pop<float>();
			float b = vm.pop<float>();
			REQUIRE(a == b);
			printf("Test executed. %.02f == %.02f\n", a, b);
			vm.currentInstruction++;
		}

		static inline Operation<test> op;

		DataType compile(CompilationContext& context, uint8_t outputPortIndex) override
		{
			if (outputPortIndex != 0)
				return DataType::Error;

			a.compile(context);
			b.compile(context);
			context.add({ op.opCode });

			return DataType::Void;
		}
	};

	struct LessThanProp : Prop
	{
		Input<float> a;
		Input<float> b;

		static void conditionLessThan(VM& vm)
		{
			// Popping A and B in reverse order so that it's more intuitive to push A and B in this order
			float b = vm.pop<float>();
			float a = vm.pop<float>();
			vm.push(a < b);
			vm.currentInstruction++;
		}

		static inline Operation<conditionLessThan> op;

		DataType compile(CompilationContext& context, uint8_t outputPortIndex) override
		{
			if (outputPortIndex != 0)
				return DataType::Error;

			a.compile(context);
			b.compile(context);
			context.add({ op.opCode });

			return DataType::Bool;
		}
	};

	struct Event
	{
		struct Action
		{
			Prop* activatedProp = nullptr;
			uint8_t actionIndex = 0;
		};

		std::vector<Action> subscribedActions;
	};

	struct EventCall : Event
	{
		void compile(CompilationContext& context)
		{
			for (auto& action : subscribedActions)
			{
				bool mustBeCompiled = context.callToFunction({ action.activatedProp->id, action.actionIndex });
				if (mustBeCompiled)
				{
					action.activatedProp->compile(context, action.actionIndex);
				}
				context.returnFromFunction();
			}
		}

		void compileIf(CompilationContext& context)
		{
			size_t jumpInstructionIndex = context.getNextInstructionIndex();
			context.add({ OpCode::JUMP_IF_NOT, 0 });

			compile(context);

			size_t endOfEventInstructionIndex = context.getNextInstructionIndex();

			// Fill in the jump size of the [jump over the else] instruction
			context.getInstruction(jumpInstructionIndex).data.data_i[0] = endOfEventInstructionIndex - jumpInstructionIndex;
		}
	};

	struct EventInlined : Event
	{
		void compile(CompilationContext& context)
		{
			for (auto& action : subscribedActions)
			{
				action.activatedProp->compile(context, action.actionIndex);
			}
		}
	};

	struct IfProp : Prop
	{
		Input<bool> condition;
		EventInlined passed;
		EventInlined failed;

		DataType compile(CompilationContext& context, uint8_t outputPortIndex) override
		{
			if (outputPortIndex != 0)
				return DataType::Error;

			condition.compile(context);

			size_t jumpToElseInstructionIndex = context.getNextInstructionIndex();
			context.add({ OpCode::JUMP_IF_NOT, 0 });

			passed.compile(context);

			size_t jumpOverElseInstructionIndex = context.getNextInstructionIndex();
			context.add({ OpCode::JUMP, 0 });

			// outputting ELSE branch
			failed.compile(context);

			size_t endOfElseInstructionIndex = context.getNextInstructionIndex();

			// Fill in the jump size of the [jump over the else] instruction
			context.getInstruction(jumpOverElseInstructionIndex).data.data_i[0] = endOfElseInstructionIndex - jumpOverElseInstructionIndex;

			// Fill in the jump size of the [jump to else] instruction
			context.getInstruction(jumpToElseInstructionIndex).data.data_i[0] = jumpOverElseInstructionIndex - jumpToElseInstructionIndex + 1;
			return DataType::Void;
		}
	};

	struct IfCallProp : Prop
	{
		Input<bool> condition;
		EventCall passed;
		EventCall failed;

		DataType compile(CompilationContext& context, uint8_t outputPortIndex) override
		{
			if (outputPortIndex != 0)
				return DataType::Error;

			condition.compile(context);

			size_t jumpToElseInstructionIndex = context.getNextInstructionIndex();
			context.add({ OpCode::JUMP_IF_NOT, 0 });

			passed.compile(context);

			size_t jumpOverElseInstructionIndex = context.getNextInstructionIndex();
			context.add({ OpCode::JUMP, 0 });

			// outputting ELSE branch
			failed.compile(context);

			size_t endOfElseInstructionIndex = context.getNextInstructionIndex();

			// Fill in the jump size of the [jump over the else] instruction
			context.getInstruction(jumpOverElseInstructionIndex).data.data_i[0] = endOfElseInstructionIndex - jumpOverElseInstructionIndex;

			// Fill in the jump size of the [jump to else] instruction
			context.getInstruction(jumpToElseInstructionIndex).data.data_i[0] = jumpOverElseInstructionIndex - jumpToElseInstructionIndex + 1;
			return DataType::Void;
		}
	};

	struct VariableSetterProp : Prop
	{
		Input<float> newValue;
		uint32_t variableId;

		static void setVariable(VM& vm)
		{
			uint32_t variableId = (uint32_t&)vm.currentInstruction->data.data_i[0];
			float newValue = vm.pop<float>();
			variables[variableId]->currentValue = newValue;
			vm.currentInstruction++;
		}

		static inline Operation<setVariable> op;

		DataType compile(CompilationContext& context, uint8_t outputPortIndex) override
		{
			if (outputPortIndex != 0)
				return DataType::Error;

			newValue.compile(context);
			context.add({ op.opCode, (int32_t)variableId });

			return DataType::Void;
		}
	};

	struct VariableGetterProp : Prop
	{
		uint32_t variableId;

		static void getVariable(VM& vm)
		{
			uint32_t variableId = (uint32_t&)vm.currentInstruction->data.data_i[0];
			vm.push(variables[variableId]->currentValue);
			vm.currentInstruction++;
		}

		static inline Operation<getVariable> opGetVariable;

		DataType compile(CompilationContext& context, uint8_t outputPortIndex) override
		{
			if (outputPortIndex != 0)
				return DataType::Error;

			context.add({ opGetVariable.opCode, (int32_t)variableId });

			return DataType::Float;
		}
	};

	struct SequenceProp : Prop
	{
		EventCall sequence;

		DataType compile(CompilationContext& context, uint8_t outputPortIndex) override
		{
			if (outputPortIndex != 0)
				return DataType::Error;

			sequence.compile(context);

			return DataType::Void;
		}
	};

	struct TickProp : Prop
	{
		EventCall tick;

		DataType compile(CompilationContext& context, uint8_t outputPortIndex) override
		{
			if (outputPortIndex != 0)
				return DataType::Error;

			tick.compile(context);

			return DataType::Void;
		}
	};


	TEST_CASE("GPP_VM", "[ECS2]")
	{
		BoilerScope boilerScope;

		/*
		* void func(float count)
		* {
		*	if(count == 0)
		*		return;
		*
		*	int i = count;
		*	while(i > 0)
		*	{
		*		print(i);
		*		i--;
		*	}
		*	func(count - 1);
		* }
		*/

		Code code;
		code.instructions =
		{
			{OpCode::PUSH, 5.0f},												 // p1
			{OpCode::CALL, 3},													 // p1
			{OpCode::POP, 4},													 // -
			{OpCode::RETURN},
			{OpCode::LOAD_PARAM, 1 * 4},									     // p1 .  x  // size 4 because this is a float
			{OpCode::PUSH, 1.0f},												 // p1 .  x 1
			{OpCode::LOAD, 0},													 // p1 .  x 1 x
			{(uint32_t)LessThanProp::op},											 // p1 .  x ?
			{OpCode::JUMP_IF_NOT, 14},										     // p1 .  x			// if( !(1 < x) ) goto...
			{OpCode::LOAD, 0},													 // p1 .  x x
			{OpCode::JUMP_IF_EQUALS, 0, 8},										 // p1 .  x
			{OpCode::LOAD, 0},													 // p1 .  x x
			{(uint32_t)PrintProp::op},												 // p1 .  x
			{OpCode::LOAD, 0},													 // p1 .  x x
			{OpCode::PUSH, -1.0f},												 // p1 .  x x -1
			{(uint32_t)AddProp::op},													 // p1 .  x xx
			{OpCode::STORE, 0},													 // p1 .  xx
			{OpCode::JUMP, -8},													 // p1 .  xx jump to second load
			{OpCode::LOAD_PARAM, 1 * 4},										 // p1 .  xx p1
			{OpCode::PUSH, -1.0f},												 // p1 .  xx p1 -1
			{(uint32_t)AddProp::op},													 // p1 .  xx pp
			{OpCode::CALL, -17},												 // p1 .  xx pp jump to first load
			{OpCode::POP, 4},													 // p1 .  xx (popping the parameter for the called function)
			{OpCode::POP, 4},													 // p1 .  (cleaning up our own local variable)
			{OpCode::RETURN},													 //
		};

		VM vm;
		vm.run(code);
	}

	// Prop setup:
	// Add1 (a = 5, b = 6)
	// Add2 (a = Add1.result, b = 10)
	// Test (input = Add2.result, update = Tick)

	// We should generate the update function of the Printer
	TEST_CASE("GPP_VM_PRINT_GOAL", "[ECS2]")
	{
		BoilerScope boilerScope;

		Code code;
		code.instructions =
		{
			{OpCode::PUSH, 5.0f},
			{OpCode::PUSH, 6.0f},
			{(uint32_t)AddProp::op},
			{OpCode::PUSH, 10.0f},
			{(uint32_t)AddProp::op},
			{OpCode::PUSH, 21.0f},
			{(uint32_t)TestProp::op},
			{OpCode::RETURN},
		};

		VM vm;
		vm.run(code);
	}

	TEST_CASE("GPP_VM_PRINT_COMPILED", "[ECS2]")
	{
		BoilerScope boilerScope;

		AddProp add1;
		add1.a.defaultValue = 5.0f;
		add1.b.defaultValue = 6.0f;

		AddProp add2;
		add2.a.sourceProp = &add1;
		add2.b.defaultValue = 10.0f;

		TestProp testProp;
		testProp.a.sourceProp = &add2;
		testProp.b.defaultValue = 21.0f;

		CompilationContext context;
		testProp.compile(context, 0);

		auto code = context.finalizeCode();
		VM vm;
		vm.run(code);
	}

	// Prop setup:
	// LessThan (a = 5, b = 1)
	// If (cond = Equals.result)
	// Test (a = 1, b = 2, update = If.passed)
	// Test (a = 2, b = 2, update = If.failed)

	// We should generate the update function of the Printer
	TEST_CASE("GPP_VM_PRINT_GOAL_2", "[ECS2]")
	{
		BoilerScope boilerScope;
		Code code;
		code.instructions = 
		{
			{OpCode::PUSH, 5.0f},
			{OpCode::PUSH, 1.0f},
			{(uint32_t)LessThanProp::op},
			{OpCode::JUMP_IF_NOT, 5},
			{OpCode::PUSH, 1.0f},	// THEN
			{OpCode::PUSH, 2.0f},
			{(uint32_t)TestProp::op},
			{OpCode::JUMP, 4},		// ELSE
			{OpCode::PUSH, 2.0f},
			{OpCode::PUSH, 2.0f},
			{(uint32_t)TestProp::op},
			{OpCode::RETURN},
		};

		VM vm;
		vm.run(code);
	}

	TEST_CASE("GPP_VM_PRINT_COMPILED_2", "[ECS2]")
	{
		BoilerScope boilerScope;


		LessThanProp ltProp;
		ltProp.a.defaultValue = 5.0f;
		ltProp.b.defaultValue = 1.0f;

		IfProp ifProp;
		ifProp.condition.sourceProp = &ltProp;
		
		TestProp testTrue;
		testTrue.a.defaultValue = 1.0f;
		testTrue.b.defaultValue = 2.0f;

		TestProp testFalse;
		testFalse.a.defaultValue = 2.0f;
		testFalse.b.defaultValue = 2.0f;

		ifProp.passed.subscribedActions.push_back({ &testTrue });
		ifProp.failed.subscribedActions.push_back({ &testFalse });

		CompilationContext context;
		ifProp.compile(context, 0);

		auto code = context.finalizeCode();
		VM vm;
		vm.run(code);
	}

	


	TEST_CASE("GPP_VM_IFCALL", "[ECS2]")
	{
		BoilerScope boilerScope;

		LessThanProp ltProp;
		ltProp.a.defaultValue = 5.0f;
		ltProp.b.defaultValue = 1.0f;

		IfCallProp ifProp;
		ifProp.condition.sourceProp = &ltProp;

		TestProp testTrue;
		testTrue.a.defaultValue = 1.0f;
		testTrue.b.defaultValue = 2.0f;

		TestProp testFalse;
		testFalse.a.defaultValue = 2.0f;
		testFalse.b.defaultValue = 2.0f;

		ifProp.passed.subscribedActions.push_back({ &testTrue });
		ifProp.failed.subscribedActions.push_back({ &testFalse });

		CompilationContext context;
		ifProp.compile(context, 0);
		context.returnFromFunction();

		auto code = context.finalizeCode();

		VM vm;
		vm.run(code);;
	}

	// Let's do a more involved test

	TEST_CASE("GPP_VM_VARSET", "[ECS2]")
	{
		BoilerScope boilerScope;

		Variable var1;
		
		VariableSetterProp variableSetProp;
		variableSetProp.newValue.defaultValue = 10.0f;
		variableSetProp.variableId = var1.id;

		VariableGetterProp variableGetProp;
		variableGetProp.variableId = var1.id;

		LessThanProp ltProp;
		ltProp.a.sourceProp = &variableGetProp;
		ltProp.b.defaultValue = 5.0f;

		TestProp trueProp;
		trueProp.a.sourceProp = &ltProp;
		trueProp.b.defaultValue = 1.0f;

		TestProp falseProp;
		falseProp.a.sourceProp = &ltProp;
		falseProp.b.defaultValue = 0.0f;

		IfProp ifProp;
		ifProp.condition.sourceProp = &ltProp;
		ifProp.passed.subscribedActions.push_back({ &trueProp, 0 });
		ifProp.failed.subscribedActions.push_back({ &falseProp, 0 });

		SequenceProp sequence;
		sequence.sequence.subscribedActions.push_back({ &ifProp, 0 });
		sequence.sequence.subscribedActions.push_back({ &variableSetProp, 0 });
		sequence.sequence.subscribedActions.push_back({ &ifProp, 0 });

		CompilationContext context;
		sequence.compile(context, 0); 
		auto code = context.finalizeCode();

		printf("Variable setter Test. Expected: 1 == 1, then 0 == 0\n\n");
		VM vm;
		vm.run(code);
	}

	struct TimerPropComponent
	{
		ECS2_COMPONENT(TimerPropComponent);
	public:
		float currentTime;
	};

	ECS2_DEFINE_COMPONENT(TimerPropComponent);


	struct TimerProp : Prop
	{
		Input<float> maxTime;
		EventCall finished;
		ecs2::EntityID timerEid;

		static void update(VM& vm)
		{
			float maxTime = vm.pop<float>();
			
			ecs2::EntityID eid(vm.currentInstruction->data.data_i[0]);

			TimerPropComponent& timerData = eid.GetComponent<TimerPropComponent>();
			float prevTime = timerData.currentTime;
			timerData.currentTime += 1.0f;

			printf("TimerData: %.2f -> %.2f\n", prevTime, timerData.currentTime);

			bool finished = false;
			if (prevTime < maxTime && timerData.currentTime >= maxTime)
			{
				finished = true;
			}

			vm.push(finished);

			vm.currentInstruction++;
		}

		static inline Operation<update> op;

		DataType compile(CompilationContext& context, uint8_t outputPortIndex) override
		{
			if (outputPortIndex != 0)
				return DataType::Error;

			maxTime.compile(context);
			context.add({ op.opCode, (int32_t&)timerEid });
			finished.compileIf(context);

			return DataType::Float;
		}
	};

	static void BoilerInit()
	{
		ecs2::Manager::Init();
		ecs2::Manager::RegisterComponent<TimerPropComponent>();
	}

	static void BoilerShutdown()
	{
		ecs2::Manager::Shutdown();
	}

	namespace
	{
		struct BoilerScope
		{
			BoilerScope() { BoilerInit(); }
			~BoilerScope() { BoilerShutdown(); }
		};
	}

	TEST_CASE("GPP_VM_TIMER", "[ECS2]")
	{
		BoilerScope boilerScope;

		ecs2::WorldID ecsWorldId = ecs2::Manager::CreateWorld();

		ecs2::ArchetypeID timerArchetype = ecs2::WorldHandle(ecsWorldId).GetArchetype<TimerPropComponent>();

		GameObjectId timerGoid = GameObjectId::createRaw(55);
		ecs2::EntityID timerEid;
		ecs2::WorldHandle(ecsWorldId).CreateEntities(timerArchetype, 1, &timerGoid, &timerEid);

		TimerProp timerProp;
		timerProp.timerEid = timerEid;
		timerProp.maxTime.defaultValue = 5.0f;
		
		TestProp trueProp;
		trueProp.a.defaultValue = 55.0f;
		trueProp.b.defaultValue = 55.0f;

		timerProp.finished.subscribedActions.push_back({ &trueProp, 0 });

		SequenceProp s;
		for (int i = 0; i < 10; i++)
		{
			s.sequence.subscribedActions.push_back({ &timerProp, 0 });
		}

		CompilationContext context;
		s.compile(context, 0);
		auto code = context.finalizeCode();

		printf("Variable setter Test. Expected: 55 == 55\n\n");
		VM vm;
		vm.run(code);
	}
}
#endif