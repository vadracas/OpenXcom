/*
 * Copyright 2010-2015 OpenXcom Developers.
 *
 * This file is part of OpenXcom.
 *
 * OpenXcom is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * OpenXcom is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with OpenXcom.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <sstream>
#include <iomanip>
#include <tuple>

#include "Logger.h"
#include "Options.h"
#include "Script.h"
#include "ScriptBind.h"
#include "Surface.h"
#include "ShaderDraw.h"
#include "ShaderMove.h"
#include "Exception.h"


namespace OpenXcom
{

////////////////////////////////////////////////////////////
//						arg definition
////////////////////////////////////////////////////////////
#define MACRO_QUOTE(...) __VA_ARGS__

#define MACRO_COPY_4(Func, Pos) \
	Func((Pos) + 0x0) \
	Func((Pos) + 0x1) \
	Func((Pos) + 0x2) \
	Func((Pos) + 0x3)
#define MACRO_COPY_16(Func, Pos) \
	MACRO_COPY_4(Func, (Pos) + 0x00) \
	MACRO_COPY_4(Func, (Pos) + 0x04) \
	MACRO_COPY_4(Func, (Pos) + 0x08) \
	MACRO_COPY_4(Func, (Pos) + 0x0C)
#define MACRO_COPY_64(Func, Pos) \
	MACRO_COPY_16(Func, (Pos) + 0x00) \
	MACRO_COPY_16(Func, (Pos) + 0x10) \
	MACRO_COPY_16(Func, (Pos) + 0x20) \
	MACRO_COPY_16(Func, (Pos) + 0x30)
#define MACRO_COPY_256(Func, Pos) \
	MACRO_COPY_64(Func, (Pos) + 0x00) \
	MACRO_COPY_64(Func, (Pos) + 0x40) \
	MACRO_COPY_64(Func, (Pos) + 0x80) \
	MACRO_COPY_64(Func, (Pos) + 0xC0)


////////////////////////////////////////////////////////////
//						proc definition
////////////////////////////////////////////////////////////
[[gnu::always_inline]]
static inline void addShade_h(int& reg, const int& var)
{
	const int newShade = (reg & 0xF) + var;
	if (newShade > 0xF)
	{
		// so dark it would flip over to another color - make it black instead
		reg = 0xF;
		return;
	}
	else if (newShade > 0)
	{
		reg = (reg & 0xF0) | newShade;
		return;
	}
	reg &= 0xF0;
	//prevent overflow to 0 or another color - make it white instead
	if (!reg || newShade < 0)
		reg = 0x01;
}

[[gnu::always_inline]]
static inline RetEnum mulAddMod_h(int& reg, const int& mul, const int& add, const int& mod)
{
	const int a = reg * mul + add;
	if (mod)
	{
		reg = (a % mod + mod) % mod;
		return RetError;
	}
	return RetContinue;
}

[[gnu::always_inline]]
static inline RetEnum wavegen_rect_h(int& reg, const int& period, const int& size, const int& max)
{
	if (period <= 0)
		return RetError;
	reg %= period;
	if (reg < 0)
		reg += reg;
	if (reg > size)
		reg = 0;
	else
		reg = max;
	return RetContinue;
}

[[gnu::always_inline]]
static inline RetEnum wavegen_saw_h(int& reg, const int& period, const int& size, const int& max)
{
	if (period <= 0)
		return RetError;
	reg %= period;
	if (reg < 0)
		reg += reg;
	if (reg > size)
		reg = 0;
	else if (reg > max)
		reg = max;
	return RetContinue;
}

[[gnu::always_inline]]
static inline RetEnum wavegen_tri_h(int& reg, const int& period, const int& size, const int& max)
{
	if (period <= 0)
		return RetError;
	reg %= period;
	if (reg < 0)
		reg += reg;
	if (reg > size)
		reg = 0;
	else
	{
		if (reg > size/2)
			reg = size - reg;
		if (reg > max)
			reg = max;
	}
	return RetContinue;
}

[[gnu::always_inline]]
static inline RetEnum call_func_h(ScriptWorker &c, FuncCommon func, const Uint8 *d, ProgPos &p)
{
	auto t = p;
	auto r = func(c, d, t);
	p = t;
	return r;
}

/**
 * Main macro defining all available operation in script engine.
 * @param IMPL macro function that access data. Take 3 args: Name, definition of operation and delcaration of it's arguments.
 */
#define MACRO_PROC_DEFINITION(IMPL) \
	/*	Name,		Implementation,													End excecution,				Args */ \
	IMPL(exit,		MACRO_QUOTE({													return RetEnd;		}),		(ScriptWorker &)) \
	\
	IMPL(ret,		MACRO_QUOTE({							c.reg[RegIn] = Data0;	return RetEnd;						}),		(ScriptWorker &c, int Data0)) \
	IMPL(ret_gt,	MACRO_QUOTE({ if (c.reg[RegCond] > 0)	{ c.reg[RegIn] = Data0; return RetEnd; } return RetContinue; }),	(ScriptWorker &c, int Data0)) \
	IMPL(ret_lt,	MACRO_QUOTE({ if (c.reg[RegCond] < 0)	{ c.reg[RegIn] = Data0; return RetEnd; } return RetContinue; }),	(ScriptWorker &c, int Data0)) \
	IMPL(ret_eq,	MACRO_QUOTE({ if (c.reg[RegCond] == 0)	{ c.reg[RegIn] = Data0; return RetEnd; } return RetContinue; }),	(ScriptWorker &c, int Data0)) \
	IMPL(ret_neq,	MACRO_QUOTE({ if (c.reg[RegCond] != 0)	{ c.reg[RegIn] = Data0; return RetEnd; } return RetContinue; }),	(ScriptWorker &c, int Data0)) \
	\
	IMPL(goto,		MACRO_QUOTE({							Prog = Label1;			return RetContinue; }),		(ScriptWorker &c, ProgPos &Prog, const ProgPos &Label1)) \
	IMPL(goto_gt,	MACRO_QUOTE({ if (c.reg[RegCond] > 0)	Prog = Label1;			return RetContinue; }),		(ScriptWorker &c, ProgPos &Prog, const ProgPos &Label1)) \
	IMPL(goto_lt,	MACRO_QUOTE({ if (c.reg[RegCond] < 0)	Prog = Label1;			return RetContinue; }),		(ScriptWorker &c, ProgPos &Prog, const ProgPos &Label1)) \
	IMPL(goto_eq,	MACRO_QUOTE({ if (c.reg[RegCond] == 0)	Prog = Label1;			return RetContinue; }),		(ScriptWorker &c, ProgPos &Prog, const ProgPos &Label1)) \
	IMPL(goto_neq,	MACRO_QUOTE({ if (c.reg[RegCond] != 0)	Prog = Label1;			return RetContinue; }),		(ScriptWorker &c, ProgPos &Prog, const ProgPos &Label1)) \
	\
	IMPL(set,		MACRO_QUOTE({							Reg0 = Data1;			return RetContinue; }),		(ScriptWorker &c, int &Reg0, int Data1)) \
	IMPL(set_gt,	MACRO_QUOTE({ if (c.reg[RegCond] > 0)	Reg0 = Data1;			return RetContinue; }),		(ScriptWorker &c, int &Reg0, int Data1)) \
	IMPL(set_lt,	MACRO_QUOTE({ if (c.reg[RegCond] < 0)	Reg0 = Data1;			return RetContinue; }),		(ScriptWorker &c, int &Reg0, int Data1)) \
	IMPL(set_eq,	MACRO_QUOTE({ if (c.reg[RegCond] == 0)	Reg0 = Data1;			return RetContinue; }),		(ScriptWorker &c, int &Reg0, int Data1)) \
	IMPL(set_neq,	MACRO_QUOTE({ if (c.reg[RegCond] != 0)	Reg0 = Data1;			return RetContinue; }),		(ScriptWorker &c, int &Reg0, int Data1)) \
	\
	IMPL(test,		MACRO_QUOTE({ c.reg[RegCond] = Data0 - Data1;					return RetContinue; }),		(ScriptWorker &c, int Data0, int Data1)) \
	IMPL(test_le,	MACRO_QUOTE({ Prog = (A <= B) ? LabelTrue : LabelFalse;			return RetContinue; }),		(ProgPos &Prog, int A, int B, const ProgPos &LabelTrue, const ProgPos &LabelFalse)) \
	IMPL(test_eq,	MACRO_QUOTE({ Prog = (A == B) ? LabelTrue : LabelFalse;			return RetContinue; }),		(ProgPos &Prog, int A, int B, const ProgPos &LabelTrue, const ProgPos &LabelFalse)) \
	\
	IMPL(swap,		MACRO_QUOTE({ std::swap(Reg0, Reg1);							return RetContinue; }),		(int &Reg0, int &Reg1)) \
	IMPL(add,		MACRO_QUOTE({ Reg0 += Data1;									return RetContinue; }),		(int &Reg0, int Data1)) \
	IMPL(sub,		MACRO_QUOTE({ Reg0 -= Data1;									return RetContinue; }),		(int &Reg0, int Data1)) \
	IMPL(mul,		MACRO_QUOTE({ Reg0 *= Data1;									return RetContinue; }),		(int &Reg0, int Data1)) \
	\
	IMPL(aggregate,	MACRO_QUOTE({ Reg0 = Reg0 + Data1 * Data2;						return RetContinue; }),		(int &Reg0, int Data1, int Data2)) \
	IMPL(offset,	MACRO_QUOTE({ Reg0 = Reg0 * Data1 + Data2;						return RetContinue; }),		(int &Reg0, int Data1, int Data2)) \
	IMPL(offsetmod,	MACRO_QUOTE({ return mulAddMod_h(Reg0, Data1, Data2, Data3);						}),		(int &Reg0, int Data1, int Data2, int Data3)) \
	\
	IMPL(div,		MACRO_QUOTE({ if (Data1) { Reg0 /= Data1; return RetError; }	return RetContinue; }),		(int &Reg0, int Data1)) \
	IMPL(mod,		MACRO_QUOTE({ if (Data1) { Reg0 %= Data1; return RetError; }	return RetContinue; }),		(int &Reg0, int Data1)) \
	\
	IMPL(shl,		MACRO_QUOTE({ Reg0 <<= Data1;									return RetContinue; }),		(int &Reg0, int Data1)) \
	IMPL(shr,		MACRO_QUOTE({ Reg0 >>= Data1;									return RetContinue; }),		(int &Reg0, int Data1)) \
	\
	IMPL(abs,		MACRO_QUOTE({ Reg0 = std::abs(Reg0);							return RetContinue; }),		(int &Reg0)) \
	IMPL(min,		MACRO_QUOTE({ Reg0 = std::min(Reg0, Data1);						return RetContinue; }),		(int &Reg0, int Data1)) \
	IMPL(max,		MACRO_QUOTE({ Reg0 = std::max(Reg0, Data1);						return RetContinue; }),		(int &Reg0, int Data1)) \
	\
	IMPL(wavegen_rect,	MACRO_QUOTE({ return wavegen_rect_h(Reg0, Data1, Data2, Data3);					}),		(int &Reg0, int Data1, int Data2, int Data3)) \
	IMPL(wavegen_saw,	MACRO_QUOTE({ return wavegen_saw_h(Reg0, Data1, Data2, Data3);					}),		(int &Reg0, int Data1, int Data2, int Data3)) \
	IMPL(wavegen_tri,	MACRO_QUOTE({ return wavegen_tri_h(Reg0, Data1, Data2, Data3);					}),		(int &Reg0, int Data1, int Data2, int Data3)) \
	\
	IMPL(get_color,		MACRO_QUOTE({ Reg0 = Data1 >> 4;							return RetContinue; }),		(int &Reg0, int Data1)) \
	IMPL(set_color,		MACRO_QUOTE({ Reg0 = (Reg0 & 0xF) | (Data1 << 4);			return RetContinue; }),		(int &Reg0, int Data1)) \
	IMPL(get_shade,		MACRO_QUOTE({ Reg0 = Data1 & 0xF;							return RetContinue; }),		(int &Reg0, int Data1)) \
	IMPL(set_shade,		MACRO_QUOTE({ Reg0 = (Reg0 & 0xF0) | (Data1 & 0xF);			return RetContinue; }),		(int &Reg0, int Data1)) \
	IMPL(add_shade,		MACRO_QUOTE({ addShade_h(Reg0, Data1);						return RetContinue; }),		(int &Reg0, int Data1)) \
	\
	IMPL(call,			MACRO_QUOTE({ return call_func_h(c, func, d, p);								}),		(FuncCommon func, const Uint8 *d, ScriptWorker &c, ProgPos &p))


////////////////////////////////////////////////////////////
//					function definition
////////////////////////////////////////////////////////////

namespace
{

/**
 * Macro returning name of function
 */
#define MACRO_FUNC_ID(id) Func_##id

/**
 * Macro used for creating functions from MACRO_PROC_DEFINITION
 */
#define MACRO_CREATE_FUNC(NAME, Impl, Args) \
	struct MACRO_FUNC_ID(NAME) \
	{ \
		[[gnu::always_inline]] \
		static RetEnum func Args \
			Impl \
	};

MACRO_PROC_DEFINITION(MACRO_CREATE_FUNC)

#undef MACRO_CREATE_FUNC

} //namespace

////////////////////////////////////////////////////////////
//					Proc_Enum definition
////////////////////////////////////////////////////////////

/**
 * Macro returning enum form ProcEnum
 */
#define MACRO_PROC_ID(id) Proc_##id

/**
 * Macro used for creating ProcEnum from MACRO_PROC_DEFINITION
 */
#define MACRO_CREATE_PROC_ENUM(NAME, ...) \
	MACRO_PROC_ID(NAME), \
	Proc_##NAME##_end = MACRO_PROC_ID(NAME) + FuncGroup<MACRO_FUNC_ID(NAME)>::ver() - 1,

/**
 * Enum storing id of all avaliable operations in script engine
 */
enum ProcEnum : Uint8
{
	MACRO_PROC_DEFINITION(MACRO_CREATE_PROC_ENUM)
	Proc_EnumMax,
};

#undef MACRO_CREATE_PROC_ENUM

////////////////////////////////////////////////////////////
//					core loop function
////////////////////////////////////////////////////////////

/**
 * Core function in script engine used to executing scripts
 * @param in arg that is visible in script under name "in"
 * @param proc array storing operation of script
 * @return Result of executing script
 */
static inline Uint8 scriptExe(int in, ScriptWorker &data)
{
	data.reg[RegIn] = in;
	ProgPos curr = {};
	const Uint8 *const proc = data.proc;
	//--------------------------------------------------
	//			helper macros for this function
	//--------------------------------------------------
	#define MACRO_FUNC_ARRAY(NAME, ...) + FuncGroup<MACRO_FUNC_ID(NAME)>::FuncList{}
	#define MACRO_FUNC_ARRAY_LOOP(POS) \
		case (POS): \
		{ \
			using currType = GetType<func, POS>; \
			const auto p = proc + (int)curr; \
			curr += currType::offset; \
			const auto ret = currType::func(data, p, curr); \
			if (ret != RetContinue) \
			{ \
				if (ret == RetEnd) \
					goto endLabel; \
				else \
					goto errorLabel; \
			} \
			else \
				continue; \
		}
	//--------------------------------------------------

	using func = decltype(MACRO_PROC_DEFINITION(MACRO_FUNC_ARRAY));

	while (true)
	{
		switch (proc[(int)curr++])
		{
		MACRO_COPY_256(MACRO_FUNC_ARRAY_LOOP, 0)
		}
	}

	//--------------------------------------------------
	//			removing helper macros
	//--------------------------------------------------
	#undef MACRO_FUNC_ARRAY_LOOP
	#undef MACRO_FUNC_ARRAY
	//--------------------------------------------------

	endLabel: return data.reg[RegIn];
	errorLabel: throw Exception("Invaild script operation!");
}


////////////////////////////////////////////////////////////
//						Script class
////////////////////////////////////////////////////////////

namespace
{

/**
 * struct used to bliting script
 */
struct ScriptReplace
{
	static inline void func(Uint8& dest, const Uint8& src, ScriptWorker& ref)
	{
		if (src)
		{
			const int s = scriptExe(src, ref);
			if (s) dest = s;
		}
	}
};

} //namespace



/**
 * Bliting one surface to another using script
 * @param src source surface
 * @param dest destination surface
 * @param x x offset of source surface
 * @param y y offset of source surface
 */
void ScriptWorker::executeBlit(Surface* src, Surface* dest, int x, int y, bool half)
{
	ShaderMove<Uint8> srcShader(src, x, y);
	if (half)
	{
		GraphSubset g = srcShader.getDomain();
		g.beg_x = g.end_x/2;
		srcShader.setDomain(g);
	}
	if (proc)
		ShaderDraw<ScriptReplace>(ShaderSurface(dest, 0, 0), srcShader, ShaderScalar(*this));
	else
		ShaderDraw<helper::StandardShade>(ShaderSurface(dest, 0, 0), srcShader, ShaderScalar(shade));
}
////////////////////////////////////////////////////////////
//					ScriptParser class
////////////////////////////////////////////////////////////

template<Uint8 procId, typename FuncGroup>
static bool parseLine(const ScriptParserData &spd, ParserHelper &ph, const SelectedToken *begin, const SelectedToken *end)
{
	auto opPos = ph.pushProc(procId);
	int ver = FuncGroup::parse(ph, begin, end);
	if (ver >= 0)
	{
		ph.updateProc(opPos, ver);
		return true;
	}
	else
	{
		return false;
	}
}

static inline bool parseCustomFunc(const ScriptParserData &spd, ParserHelper &ph, const SelectedToken *begin, const SelectedToken *end)
{
	using argFunc = typename ArgSelector<FuncCommon>::type;
	using argRaw = typename ArgSelector<const Uint8*>::type;
	static_assert(FuncGroup<Func_call>::ver() == argRaw::ver(), "Invalid size");
	static_assert(std::is_same<GetType<FuncGroup<Func_call>, 0>, argFunc>::value, "Invalid first argument");
	static_assert(std::is_same<GetType<FuncGroup<Func_call>, 1>, argRaw>::value, "Invalid second argument");

	auto opPos = ph.pushProc(Proc_call);

	auto funcPos = ph.pushReserved<FuncCommon>();
	auto argPosBegin = ph.getCurrPos();

	auto argType = spd.arg(ph, begin, end);

	if (argType < 0)
	{
		return false;
	}

	auto argPosEnd = ph.getCurrPos();
	ph.updateReserved<FuncCommon>(funcPos, spd.get(argType));

	size_t diff = ph.getDiffPos(argPosBegin, argPosEnd);
	for (int i = 0; i < argRaw::ver(); ++i)
	{
		size_t off = argRaw::offset(i);
		if (diff <= off)
		{
			//aligin proc to fit fixed size.
			ph.proc.insert(ph.proc.end(), off-diff, 0);
			ph.updateProc(opPos, i);
			return true;
		}
	}
	return false;
}

static bool parseConditionImpl(ParserHelper &ph, int nextPos, const SelectedToken *begin, const SelectedToken *end)
{
	constexpr size_t TempArgsSize = 4;
	constexpr size_t OperatorSize = 6;

	if (std::distance(begin, end) != 3)
	{
		Log(LOG_ERROR) << "invaild length of condition arguments\n";
		return false;
	}

	const auto currPos = ph.addLabel();
	const auto dummy = std::string{};
	const auto dummyIter = std::end(dummy);

	const auto conditionType = begin[0].toString();
	SelectedToken conditionArgs[TempArgsSize] =
	{
		begin[1],
		begin[2],
		{ TokenBuildinLabel, dummyIter, dummyIter, currPos, }, //success
		{ TokenBuildinLabel, dummyIter, dummyIter, nextPos, }, //failure
	};

	const char *opNames[OperatorSize] =
	{
		"eq", "neq",
		"le", "gt",
		"ge", "lt",
	};

	bool equalFunc = false;
	int i = 0;
	for (; i < OperatorSize; ++i)
	{
		if (conditionType.compare(opNames[i]) == 0)
		{
			if (i < 2) equalFunc = true;
			if (i & 1) std::swap(conditionArgs[2], conditionArgs[3]); //negate condition result
			if (i >= 4) std::swap(conditionArgs[0], conditionArgs[1]); //swap condition args
			break;
		}
	}
	if (i == OperatorSize)
	{
		Log(LOG_ERROR) << "unknown condition: '" + conditionType + "'\n";
		return false;
	}

	auto proc = ph.pushProc(equalFunc ? Proc_test_eq : Proc_test_le);
	auto argType = 0;

	if (equalFunc)
	{
		argType = FuncGroup<Func_test_eq>::parse(ph, std::begin(conditionArgs), std::end(conditionArgs));
	}
	else
	{
		argType = FuncGroup<Func_test_le>::parse(ph, std::begin(conditionArgs), std::end(conditionArgs));
	}

	if (argType < 0)
	{
		return false;
	}
	ph.updateProc(proc, argType);

	ph.setLabel(currPos, ph.getCurrPos());

	return true;
}

static bool parseIf(const ScriptParserData &spd, ParserHelper &ph, const SelectedToken *begin, const SelectedToken *end)
{
	ParserHelper::Block block = { BlockIf, ph.addLabel(), ph.addLabel() };
	ph.codeBlocks.push_back(block);

	return parseConditionImpl(ph, block.nextLabel, begin, end);
}

static bool parseElse(const ScriptParserData &spd, ParserHelper &ph, const SelectedToken *begin, const SelectedToken *end)
{
	if (ph.codeBlocks.empty() || ph.codeBlocks.back().type != BlockIf)
	{
		Log(LOG_ERROR) << "unexpected 'else'\n";
		return false;
	}

	ParserHelper::Block &block = ph.codeBlocks.back();

	ph.pushProc(Proc_goto);
	ph.pushLabel(block.finalLabel);

	ph.setLabel(block.nextLabel, ph.getCurrPos());
	if (std::distance(begin, end) == 0)
	{
		block.nextLabel = block.finalLabel;
		block.type = BlockElse;
		return true;
	}
	else
	{
		block.nextLabel = ph.addLabel();
		return parseConditionImpl(ph, block.nextLabel, begin, end);
	}
}

static bool parseEnd(const ScriptParserData &spd, ParserHelper &ph, const SelectedToken *begin, const SelectedToken *end)
{
	if (ph.codeBlocks.empty())
	{
		Log(LOG_ERROR) << "unexpected 'end'\n";
		return false;
	}
	if (std::distance(begin, end) != 0)
	{
		Log(LOG_ERROR) << "unexpected symbols after 'end'\n";
		return false;
	}

	ParserHelper::Block block = ph.codeBlocks.back();
	ph.codeBlocks.pop_back();

	ph.setLabel(block.finalLabel, ph.getCurrPos());
	return true;
}

/**
 * Default constructor
 */
ScriptParserBase::ScriptParserBase(const std::string& name) : _regUsed{ RegCustom }, _name{ name }
{
	//--------------------------------------------------
	//					op_data init
	//--------------------------------------------------
	#define MACRO_ALL_INIT(NAME, ...) \
		_procList[#NAME] = { &parseLine<MACRO_PROC_ID(NAME), FuncGroup<MACRO_FUNC_ID(NAME)>> };

	MACRO_PROC_DEFINITION(MACRO_ALL_INIT)

	#undef MACRO_ALL_INIT

	_procList["if"] = { &parseIf };
	_procList["else"] = { &parseElse };
	_procList["end"] = { &parseEnd };

	addStandartReg("in", RegIn);
	addStandartReg("r0", RegR0);
	addStandartReg("r1", RegR1);
	addStandartReg("r2", RegR2);
	addStandartReg("r3", RegR3);

	addType<int>("int");
}

/**
 * Add new function parsing arguments of script operation.
 * @param s function name
 * @param parser parsing fu
 */
void ScriptParserBase::addParserBase(const std::string& s, ScriptParserData::argFunc arg, ScriptParserData::getFunc get)
{
	auto pos = _procList.find(s);
	if (pos == _procList.end())
	{
		_procList[s] =
		{
			&parseCustomFunc,
			arg,
			get,
		};
	}
	else
	{
		throw Exception("Repeated function name: '" + s + "'\n");
	}
}

void ScriptParserBase::addTypeBase(const std::string& s, ArgEnum type)
{
	auto pos = _refList.find(s);
	if (pos != _refList.end() && pos->second.type != type)
	{
		throw Exception("Type name already used: '" + s + "'\n");
	}
	_typeList[s] = type;
}

void ScriptParserBase::addStandartReg(const std::string& s, RegEnum index)
{
	ScriptContainerData data = { ArgReg, index };
	_refList.insert(std::make_pair(s, data));
}
/**
 * Set name for custom script param
 * @param i what custom param
 * @param s name for first custom parameter
 */
void ScriptParserBase::addCustomReg(const std::string& s, ArgEnum type)
{
	if (_regUsed < ScriptMaxReg)
	{
		if (_refList.find(s) != _refList.end())
		{
			throw Exception("Name already used: '" + s + "'\n");
		}
		ScriptContainerData data = { type, _regUsed++ };
		_refList.insert(std::make_pair(s, data));
	}
	else
	{
		throw Exception("Custom arg limit reach for: '" + s + "'\n");
	}
}

/**
 * Add const value to script
 * @param s name for const
 * @param i value
 */
void ScriptParserBase::addConst(const std::string& s, int i)
{
	ScriptContainerData data = { ArgConst, 0, i };
	_refList.insert(std::make_pair(s, data));
}

/**
 * Parse string and write script to ScriptBase
 * @param src struct where final script is write to
 * @param src_code string with script
 * @return true if string have valid script
 */
bool ScriptParserBase::parseBase(ScriptContainerBase* destScript, const std::string& srcCode) const
{
	ParserHelper help(
		_regUsed,
		destScript->_proc,
		_procList,
		_refList
	);
	ite curr = srcCode.begin();
	ite end = srcCode.end();
	if (curr == end)
		return false;

	while (true)
	{
		if (!help.findToken(curr, end))
		{
			if (help.regIndexUsed > ScriptMaxReg)
			{
				Log(LOG_ERROR) << "script used to many references\n";
				return false;
			}
			for (auto i = help.refListCurr.begin(); i != help.refListCurr.end(); ++i)
			{
				if (i->second.type == ArgLabel && i->second.value == -1)
				{
					Log(LOG_ERROR) << "invalid use of label: '" << i->first << "' without declaration\n";
					return false;
				}
			}
			help.relese();
			return true;
		}

		ite line_begin = curr;
		ite line_end;
		SelectedToken label = { TokenNone };

		SelectedToken op = help.getToken(curr, end);
		SelectedToken args[ScriptMaxArg];
		args[0] = help.getToken(curr, end, TokenColon);
		if (args[0].type == TokenColon)
		{
			std::swap(op, label);
			op = help.getToken(curr, end);
			args[0] = help.getToken(curr, end);
		}

		std::string op_str = op.toString();
		std::string arg_str;
		std::string label_str = label.toString();

		if (_procList.find(op_str) == _procList.end())
		{
			auto first_dot = op_str.find('.');
			if (first_dot != std::string::npos)
			{
				auto temp = op_str.substr(0, first_dot);
				auto ref = help.getReferece(temp);
				if (ref && ref->type >= ArgCustom)
				{
					for (auto& t : _typeList)
					{
						if (t.second == ref->type)
						{
							arg_str = std::move(temp);
							op_str = t.first + op_str.substr(first_dot);
							args[1] = args[0];
							args[0] = { TokenSymbol, std::begin(arg_str), std::end(arg_str) };
							break;
						}
					}
				}
			}
		}
		for (int i = (arg_str.empty() ? 1 : 2); i < ScriptMaxArg; ++i)
			args[i] = help.getToken(curr, end);
		SelectedToken f = help.getToken(curr, end, TokenSemicolon);

		line_end = curr;
		//validation
		bool valid = true;
		valid &= label.type == TokenSymbol || label.type == TokenNone;
		valid &= op.type == TokenSymbol;
		for (int i = 0; i < ScriptMaxArg; ++i)
			valid &= args[i].type == TokenSymbol || args[i].type == TokenNumber || args[i].type == TokenNone;
		valid &= f.type == TokenSemicolon;

		if (!valid)
		{
			if (f.type != TokenSemicolon)
			{
				//fixing `line_end` position
				while(curr != end && *curr != ';')
					++curr;
				if (curr != end)
					++curr;
				line_end = curr;
			}
			Log(LOG_ERROR) << "invalid line: '" << std::string(line_begin, line_end) << "'\n";
			return false;
		}
		else
		{

			//matching args form operation definition with args avaliable in string
			int i = 0;
			while (i < ScriptMaxArg && args[i].type != TokenNone)
				++i;

			if (!label_str.empty() && !help.setLabel(label_str, help.getCurrPos()))
			{
				Log(LOG_ERROR) << "invalid label '"<< label_str <<"' in line: '" << std::string(line_begin, line_end) << "'\n";
				return false;
			}

			auto op_curr = _procList.find(op_str);
			if (op_curr != _procList.end())
			{
				if (op_curr->second(help, args, args+i) == false)
				{
					Log(LOG_ERROR) << "invalid line: '" << std::string(line_begin, line_end) << "'\n";
					return false;
				}

				continue;
			}

			auto type_curr = _typeList.find(op_str);
			if (type_curr != _typeList.end())
			{
				for (int j = 0; j < i; ++j)
				{
					std::string var_name = args[j].toString();
					if (args[j].type != TokenSymbol || help.addReg(var_name, type_curr->second) == false)
					{
						Log(LOG_ERROR) << "invalid variable name '"<< var_name <<"' in line: '" << std::string(line_begin, line_end) << "'\n";
						return false;
					}
				}

				continue;
			}

			Log(LOG_ERROR) << "invalid operation name '"<< op_str <<"' in line: '" << std::string(line_begin, line_end) << "'\n";
			return false;
		}
	}
}

/**
 * Print all metadata
 */
void ScriptParserBase::logScriptMetadata() const
{
	if (Options::debug)
	{
		const int tabSize = 8;
		static bool printOp = true;
		if (printOp)
		{
			printOp = false;
			Logger opLog;
			#define MACRO_STRCAT(A) #A
			#define MACRO_ALL_LOG(NAME, Impl, Args) \
				if (#NAME[0] != '_') opLog.get() \
					<< "Op:    " << std::setw(tabSize*2) << #NAME \
					<< "Impl:  " << std::setw(tabSize*10) << MACRO_STRCAT(Impl) \
					<< "Args:  " #Args << FuncGroup<MACRO_FUNC_ID(NAME)>::ver() << "\n";

			opLog.get() << "Available script operations:\n" << std::left;
			MACRO_PROC_DEFINITION(MACRO_ALL_LOG)

			#undef MACRO_ALL_LOG
			#undef MACRO_STRCAT
		}

		Logger refLog;
		refLog.get() << "Script data for: " << _name << "\n" << std::left;
		for (auto ite = _refList.begin(); ite != _refList.end(); ++ite)
		{
			if (ite->second.type == ArgConst)
				refLog.get() << "Ref: " << std::setw(30) << ite->first << "Value: " << ite->second.value << "\n";
			else
				refLog.get() << "Ref: " << std::setw(30) << ite->first << "\n";
		}
	}
}

} //namespace OpenXcom
