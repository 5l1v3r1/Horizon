#ifndef _DEBUG

#include <windows.h>
#include <typeinfo>
#include "Excpt.h"

#pragma warning (disable: 4731) // ebp register overwritten
#pragma warning (disable: 4733) // fs:[0] accessed
#pragma warning (disable: 4200) // zero-sized arrays
#pragma warning (disable: 4094)	// untagged class

#ifdef _DEBUG
#	define ASSERT(x) do { if (!(x)) __debugbreak(); } while (false)
#else
#	define ASSERT(x) __noop
#endif

template <bool ok> struct AssertTest;
template <> struct AssertTest<true> {};
#define STATIC_ASSERT(x) class { class AssertTstAtLine##__LINE__ :public AssertTest<(bool) (x)> {}; }


inline void Verify(bool bVal)
{
	if (!bVal) __debugbreak();
}

//#define CHKREG_ENABLE

#ifdef CHKREG_ENABLE

#	define CHKREG_PROLOG_NO_EBX \
	{ \
		_asm push ebp \
		_asm push esi \
		_asm push edi \
	}

#	define CHKREG_PROLOG \
	{ \
		_asm push ebx \
	} \
	CHKREG_PROLOG_NO_EBX


#	define CHKREG_EPILOG_REG(reg) \
		_asm cmp reg, [esp] \
		_asm je Ok##reg \
		_asm int 3 \
		_asm Ok##reg: \
		_asm add esp, 4

#	define CHKREG_EPILOG_NO_EBX \
	{ \
		CHKREG_EPILOG_REG(edi) \
		CHKREG_EPILOG_REG(esi) \
		CHKREG_EPILOG_REG(ebp) \
	}

#	define CHKREG_EPILOG \
	CHKREG_EPILOG_NO_EBX \
	{ \
		CHKREG_EPILOG_REG(ebx) \
	}


#else // CHKREG_ENABLE

#	define CHKREG_PROLOG_NO_EBX
#	define CHKREG_PROLOG
#	define CHKREG_EPILOG_NO_EBX
#	define CHKREG_EPILOG

#endif // CHKREG_ENABLE

////////////////////////////////////////
// Exception registration record (not only C++)
struct ExcReg
{
	ExcReg* mPPrev;
	PVOID	mPfnHandler;

	inline static ExcReg* get_Top();
	inline void put_Top(); // can call even for NULL obj

	void Install() {
		mPPrev = get_Top();
		put_Top();
	}

	void Dismiss() {
		Verify(this == get_Top());
		mPPrev->put_Top();
	}

	[[nodiscard]] bool IsValid() const { return this != reinterpret_cast<ExcReg*>(-1); }
};

ExcReg* ExcReg::get_Top()
{
	ExcReg* pExcReg;
	_asm
	{
		mov eax, fs: [0]
		mov pExcReg, eax
	}
	return pExcReg;
}

void ExcReg::put_Top() // can call even for NULL obj
{
	auto pExcReg = this;
	_asm
	{
		mov eax, pExcReg;
		mov fs : [0] , eax
	}
}

static const DWORD EXC_FLAG_UNWIND = 6;

namespace Exc
{
	class MonitorRaw {
	protected:
		DWORD m_pOpaque[2]; // impl details
		static EXCEPTION_DISPOSITION HandlerStat(EXCEPTION_RECORD* pExc, PVOID, CONTEXT* pCpuCtx);
	public:

		MonitorRaw();

		virtual bool Handle(EXCEPTION_RECORD* pExc, CONTEXT* pCpuCtx) { return false; }
		virtual void AbnormalExit() {}

		void NormalExit(); // *MUTS* be called upon normal exit!!!
	};



	struct Monitor :public MonitorRaw {
		// Automatically handles scope exit
		~Monitor();
	};

	void RaiseExc(EXCEPTION_RECORD&, CONTEXT* = nullptr) noexcept;

	EXCEPTION_RECORD* GetCurrentExc();

	void SetFrameHandler(bool bSet);
	void SetThrowFunction(bool bSet);
	void SetUncaughtExc(bool bSet);

} // namespace Exc

EXCEPTION_DISPOSITION Exc::MonitorRaw::HandlerStat(EXCEPTION_RECORD* pExc, PVOID pPtr, CONTEXT* pCpuCtx)
{
	ASSERT(pPtr);
	const auto pExcReg = static_cast<ExcReg*>(pPtr);
	auto pThis = reinterpret_cast<Monitor*>(PBYTE(pPtr) - offsetof(Monitor, m_pOpaque));

	if (EXC_FLAG_UNWIND & pExc->ExceptionFlags)
	{
		pExcReg->mPfnHandler = nullptr; // dismissed
		pThis->AbnormalExit();
	}
	else
		if (pThis->Handle(pExc, pCpuCtx))
		{
			ASSERT(!(EXCEPTION_NONCONTINUABLE & pExc->ExceptionFlags));
			return ExceptionContinueExecution;
		}

	return ExceptionContinueSearch;

}

Exc::MonitorRaw::MonitorRaw()
{
	STATIC_ASSERT(sizeof(m_pOpaque) == sizeof(ExcReg));
	reinterpret_cast<ExcReg&>(m_pOpaque).mPfnHandler = HandlerStat;
	reinterpret_cast<ExcReg&>(m_pOpaque).Install();

}

void Exc::MonitorRaw::NormalExit()
{
	STATIC_ASSERT(sizeof(m_pOpaque) == sizeof(ExcReg));
	ASSERT(((ExcReg&)m_pOpaque).m_pfnHandler);
	reinterpret_cast<ExcReg&>(m_pOpaque).Dismiss();
}


Exc::Monitor::~Monitor()
{
	STATIC_ASSERT(sizeof(m_pOpaque) == sizeof(ExcReg));
	if (reinterpret_cast<ExcReg&>(m_pOpaque).mPfnHandler)
		reinterpret_cast<ExcReg&>(m_pOpaque).Dismiss();
}

////////////////////////////////////////
// Exception registration record built by C++ compiler
struct ExcRegCpp
	:public ExcReg
{
	union {
		long	mNextCleanup;	// if used for cleanup
		DWORD	mDwTryId;		// if used for search
	};
	ULONG	mNEbpPlaceholder;
};

////////////////////////////////////////
// Thrown type description (_s__ThrowInfo)
struct ThrownType
{
	DWORD	u1;
	PVOID	mPfnDtor;
	DWORD	u2;

	struct Sub
	{
		DWORD		u1;
		type_info* ti;
		DWORD		u2[3];
		DWORD		mNSize;
		PVOID		mPfnCopyCtor;
	};

	struct Table {
		long mNCount;
		ThrownType::Sub* mPArr[];
	} *mPTable;
};

////////////////////////////////////////
// Frame info, generated by C++ compiler for __CxxFrameHandler3
struct FrameInfo
{
	ULONG			mSig;

	template <typename T> struct ArrT {
		long	mNCount;
		T* mPArr;
	};

	// Cleanup data
	struct Cleanup {
		long	mNPrevIdx;
		PVOID	mPfnCleanup; // __stdcall, takes no params
	};
	ArrT<Cleanup> m_Cleanup;

	// Try blocks
	struct Try {
		DWORD	mIdStart;
		DWORD	mIdEnd;
		DWORD	mU1;

		// Catch blocks
		struct Catch {
			DWORD		mValOrRef;
			type_info* mTi;
			int			mOffset;	// from EBP where exception should be copied.
			DWORD		mCatchblockAddr;
		};
		ArrT<Catch> mCatch;
	};
	ArrT<Try> mTry;

	EXCEPTION_DISPOSITION __thiscall Handler(EXCEPTION_RECORD* pExc, ExcRegCpp* pExcRegCpp, CONTEXT* pCpuCtx);
};


// Worker struct
struct WorkerForLocalUnwind
{
	FrameInfo* mPFrame;
	ExcRegCpp* mPExcRegCpp;
	ULONG		mNEbp;

	void UnwindLocal(long nStopAtId);
	void UnwindLocalWithGuard();
};

struct WrapExcRecord
{
	EXCEPTION_RECORD* mPExc;
	CONTEXT* mPCpuCtx;

	[[nodiscard]] EXCEPTION_DISPOSITION InvokeHandler(const ExcReg&) const;
	void RaiseExcRaw(ExcReg* pExcReg) const noexcept;
	void Unhandled() const;
};



// Last exception information (per-thread)
// This structure is used to support nested exceptions and "rethrow"
struct ExcMon
	:public ExcReg
	, public WrapExcRecord
{
	__declspec(thread) static ExcMon* sPInst;

	static EXCEPTION_DISPOSITION Handler(EXCEPTION_RECORD* pExc, ExcMon* pThis, CONTEXT* pCpuCtx) {
		ASSERT(pExc && pThis);
		if (EXC_FLAG_UNWIND & pExc->ExceptionFlags)
			pThis->RemoveLastExc();
		return ExceptionContinueSearch;
	}

	ExcMon(EXCEPTION_RECORD* pExc, CONTEXT* pCpuCtx) : ExcReg(), WrapExcRecord() {
		mPfnHandler = Handler;
		Install();

		mPExc = pExc;
		mPCpuCtx = pCpuCtx;
		sPInst = this;
	}

	void AppendExc(EXCEPTION_RECORD* pExc)
	{
		ASSERT(pExc && (pExc != m_pExc));
		pExc->ExceptionRecord = mPExc;
		mPExc = pExc;
	}

	void RemoveLastExc()
	{
		Verify(this == sPInst);
		sPInst = nullptr;
	}

	void NormalExit()
	{
		RemoveLastExc();
		ExcReg::Dismiss();
	}
};

__declspec(thread) ExcMon* ExcMon::sPInst = nullptr;



struct WorkerForAll
	:public WorkerForLocalUnwind
{
	ThrownType* mPThrownType;
	PVOID		mPThrownObj;

	void Catch();
	void CatchInternal(const FrameInfo::Try&, const FrameInfo::Try::Catch&);
	void CatchInternalFinal(const FrameInfo::Try& tryEntry, const FrameInfo::Try::Catch& catchEntry, const ThrownType::Sub*);

	void UnwindUntil(const FrameInfo::Try&);

	void AssignCatchObj(const FrameInfo::Try::Catch&, const ThrownType::Sub&) const;
	DWORD CallCatchRaw(const FrameInfo::Try::Catch&) const;
	void DestroyThrownObjSafe() const;

	__declspec(noreturn) void PassCtlAfterCatch(DWORD dwAddr) const;
};


void WorkerForLocalUnwind::UnwindLocal(long nStopAtId)
{
	const FrameInfo::Cleanup* pCleanupArr = mPFrame->m_Cleanup.mPArr;
	auto nEbp = mNEbp;

	while (true)
	{
		auto nNextId = mPExcRegCpp->mNextCleanup;
		if (nNextId <= nStopAtId)
			break;

		Verify(nNextId < mPFrame->m_Cleanup.mNCount); // stack corruption test

		const auto& entry = pCleanupArr[nNextId];
		auto pfnCleanup = entry.mPfnCleanup;
		mPExcRegCpp->mNextCleanup = entry.mNPrevIdx;

		if (pfnCleanup)
			_asm
		{
			mov eax, pfnCleanup
			mov ebx, ebp	// save current ebp

			CHKREG_PROLOG

			push esi
			push edi

			mov ebp, nEbp	// the ebp of the unwinding function
			call eax
			mov ebp, ebx	// restore ebp

			pop edi
			pop esi

			CHKREG_EPILOG
		}
	}
}

void WorkerForLocalUnwind::UnwindLocalWithGuard()
{
	struct Monitor_Guard :public Exc::MonitorRaw {
		WorkerForLocalUnwind* m_pWrk;

		void AbnormalExit() override {
			m_pWrk->UnwindLocalWithGuard(); // recursively
		}
	};

	Monitor_Guard mon;
	mon.m_pWrk = this;

	UnwindLocal(-1);

	// dismiss
	mon.NormalExit();
}

EXCEPTION_DISPOSITION WrapExcRecord::InvokeHandler(const ExcReg& excReg) const
{
	auto pExc = mPExc;
	auto pCpuCtx = mPCpuCtx;
	auto pExcReg = &excReg;
	auto pfnHandler = excReg.mPfnHandler;
	ASSERT(pfnHandler);

	EXCEPTION_DISPOSITION retVal;

	_asm
	{
		// There's some ambiguity here: Some handlers are actually __cdecl funcs, whereas
		// others are __stdcall. So we have no guarantee about the state of the stack.
		// Hence - we save it manually in ebx register (it remains valid across function call).

		CHKREG_PROLOG_NO_EBX

		mov ebx, esp	// save esp

		push 0
		push pCpuCtx
		push pExcReg
		push pExc
		call pfnHandler

		mov esp, ebx	// restore esp
		mov retVal, eax

		CHKREG_EPILOG_NO_EBX
	}

	return retVal;
}

void WrapExcRecord::RaiseExcRaw(ExcReg* pExcReg) const noexcept {
	const EXCEPTION_RECORD* pExc = mPExc;

	for (; ; pExcReg = pExcReg->mPPrev)
	{
		if (!pExcReg->IsValid())
			Unhandled();

		if (ExceptionContinueExecution == InvokeHandler(*pExcReg))
		{
			Verify(!(EXCEPTION_NONCONTINUABLE & mPExc->ExceptionFlags));
			break;
		}
	}
}

void WrapExcRecord::Unhandled() const
{
	EXCEPTION_POINTERS excPt;
	excPt.ExceptionRecord = mPExc;
	excPt.ContextRecord = mPCpuCtx;
	UnhandledExceptionFilter(&excPt);
}

void WorkerForAll::UnwindUntil(const FrameInfo::Try& tryEntry)
{
	// Global unwind
	EXCEPTION_RECORD exc;
	exc.ExceptionCode = 0;
	exc.ExceptionFlags = EXC_FLAG_UNWIND;
	exc.ExceptionRecord = nullptr; // ???
	exc.ExceptionAddress = 0;
	exc.NumberParameters = 0;

	static CONTEXT ctx = { 0 };

	WrapExcRecord wrk{};
	wrk.mPExc = &exc;
	wrk.mPCpuCtx = &ctx;

	ExcReg* pParent = nullptr;
	for (auto pExcReg = ExcReg::get_Top(); mPExcRegCpp != pExcReg; )
	{
		Verify(pExcReg->IsValid());
		auto pPrev = pExcReg->mPPrev;

		if (pExcReg->mPfnHandler == ExcMon::Handler)
			pParent = pExcReg;
		else
		{
			// dismiss it before calling the handler. This is the convention.
			if (pParent)
				pParent->mPPrev = pPrev;
			else
				pPrev->put_Top();

			// ReSharper disable once CppExpressionWithoutSideEffects
			// ReSharper disable once CppNoDiscardExpression
			wrk.InvokeHandler(*pExcReg);
		}

		pExcReg = pPrev;
	}

	// Local unwind
	UnwindLocal(tryEntry.mIdStart - 1);
}

void WorkerForAll::AssignCatchObj(const FrameInfo::Try::Catch& catchEntry, const ThrownType::Sub& typeSub) const {
	ASSERT(m_pThrownType);

	if (catchEntry.mOffset)
	{
		auto pDst = mNEbp + catchEntry.mOffset;
		auto pObj = mPThrownObj;

		// copy exc there
		switch (8 & catchEntry.mValOrRef)
		{
		case 8: // by ref
			ASSERT(!(pDst & 3)); // should be DWORD-aligned, don't care if it's not
			*reinterpret_cast<PVOID*>(pDst) = pObj;
			break;

		case 0: // by val
		{
			auto pfnCtor = typeSub.mPfnCopyCtor;
			if (pfnCtor)
			{
				_asm
				{
					mov  ecx, pDst	// this

					CHKREG_PROLOG

					push pObj	// arg
					call pfnCtor

					CHKREG_EPILOG
				}
			}
			else
				CopyMemory(PVOID(pDst), pObj, typeSub.mNSize);
		}
		default:;
		}
	}
}

DWORD WorkerForAll::CallCatchRaw(const FrameInfo::Try::Catch& catchEntry) const {
	auto dwAddr = catchEntry.mCatchblockAddr;
	Verify(0 != dwAddr);

	auto dwEbp = mNEbp;

	_asm
	{
		mov eax, dwAddr
		mov ebx, ebp	// save ebp

		CHKREG_PROLOG

		mov ebp, dwEbp

		// 'Catch' doesn't save esi, edi
		push esi
		push edi

		call eax

		pop edi
		pop esi
		mov ebp, ebx	// restore ebp

		CHKREG_EPILOG

		mov dwAddr, eax // return value (eax) is the address where to pass control to
	}

	return dwAddr;
}

__declspec(noreturn) void WorkerForAll::PassCtlAfterCatch(DWORD dwAddr) const {
	ASSERT(dwAddr);
	ULONG nEbp = mNEbp;
	_asm
	{
		mov eax, dwAddr
		mov ebp, nEbp
		mov esp, [ebp - 10h] // ?!?
		jmp eax
	}
}

void WorkerForAll::DestroyThrownObjSafe() const {
	if (mPThrownType && mPThrownType->mPfnDtor)
	{
		ASSERT(m_pThrownObj);

		auto pObj = mPThrownObj;
		auto pfnDtor = mPThrownType->mPfnDtor;

		_asm
		{
			mov ecx, pObj

			CHKREG_PROLOG
			call pfnDtor
			CHKREG_EPILOG
		}
	}
}

void WorkerForAll::Catch()
{
	const auto nTryCount = mPFrame->mTry.mNCount;
	if (nTryCount > 0)
	{
		const auto dwTryId = mPExcRegCpp->mDwTryId;
		const FrameInfo::Try* pTryTable = mPFrame->mTry.mPArr;
		Verify(nullptr != pTryTable);

		// find the enclosing try block
		long nTry = 0;
		do
		{
			const auto& tryEntry = mPFrame->mTry.mPArr[nTry];
			const auto nCatchCount = tryEntry.mCatch.mNCount;

			if ((dwTryId >= tryEntry.mIdStart) &&
				(dwTryId <= tryEntry.mIdEnd) &&
				(nCatchCount > 0))
			{
				// find the appropriate catch block
				const FrameInfo::Try::Catch* pCatch = tryEntry.mCatch.mPArr;
				Verify(nullptr != pCatch);

				long nCatch = 0;
				do {
					CatchInternal(tryEntry, pCatch[nCatch]);
				} while (++nCatch < nCatchCount);
			}

		} while (++nTry < nTryCount);
	}
}

void WorkerForAll::CatchInternal(const FrameInfo::Try& tryEntry, const FrameInfo::Try::Catch& catchEntry)
{
	// all the current exceptions
	for (auto ppExc = &ExcMon::sPInst->mPExc; ; )
	{
		auto pExc = *ppExc;
		ASSERT(pExc);

		// get the C++ exception info
		if ((0xe06d7363 == pExc->ExceptionCode) &&
			(3 == pExc->NumberParameters))
		{
			mPThrownObj = reinterpret_cast<PVOID>(pExc->ExceptionInformation[1]);
			mPThrownType = reinterpret_cast<ThrownType*>(pExc->ExceptionInformation[2]);

			Verify(
				mPThrownObj &&
				mPThrownType &&
				mPThrownType->mPTable &&
				(mPThrownType->mPTable->mNCount > 0));

			long nSub = 0, nSubTotal = mPThrownType->mPTable->mNCount;
			const auto ppSub = mPThrownType->mPTable->mPArr;

			do {
				const auto pSub = ppSub[nSub];
				Verify(pSub && nullptr != pSub->ti);
			} while (++nSub < nSubTotal);

		}
		else
			mPThrownType = nullptr;

		if (catchEntry.mTi)
		{
			if (mPThrownType)
			{
				// Check if this type can be cast to the dst one
				long nSub = 0;
				const auto nSubTotal = mPThrownType->mPTable->mNCount;
				const auto ppTypeSub = mPThrownType->mPTable->mPArr;

				while (true)
				{
					const auto& typeSub = *(ppTypeSub[nSub]);
					if (*catchEntry.mTi == *typeSub.ti)
					{
						CatchInternalFinal(tryEntry, catchEntry, &typeSub);
						*ppExc = pExc->ExceptionRecord;
						break;
					}

					if (++nSub == nSubTotal)
					{
						ppExc = &pExc->ExceptionRecord;
						break;
					}
				}
			}

		}
		else
		{
			CatchInternalFinal(tryEntry, catchEntry, nullptr);
			*ppExc = pExc->ExceptionRecord;
		}

		if (!*ppExc)
			break;
	}
}

void WorkerForAll::CatchInternalFinal(const FrameInfo::Try& tryEntry, const FrameInfo::Try::Catch& catchEntry, const ThrownType::Sub* pTypeSub)
{
	// Match!
	UnwindUntil(tryEntry);

	if (pTypeSub)
		AssignCatchObj(catchEntry, *pTypeSub);
	else
		Verify(!catchEntry.mOffset);

	const auto dwAddr = CallCatchRaw(catchEntry);

	mPExcRegCpp->mDwTryId++; // if exc is re-raised now - give this try block opportunity to handle it
	DestroyThrownObjSafe();

	if (!ExcMon::sPInst->mPExc->ExceptionRecord)
	{
		// finito
		ExcMon::sPInst->NormalExit();
		PassCtlAfterCatch(dwAddr);
	}

}


EXCEPTION_DISPOSITION __thiscall FrameInfo::Handler(EXCEPTION_RECORD* pExc, ExcRegCpp* pExcRegCpp, CONTEXT* pCpuCtx)
{
	Verify(this && pExc && pExcRegCpp && pCpuCtx);

	WorkerForAll wrk{};
	wrk.mPFrame = this;
	wrk.mPExcRegCpp = pExcRegCpp;
	wrk.mNEbp = static_cast<ULONG>(reinterpret_cast<ULONG_PTR>(&pExcRegCpp->mNEbpPlaceholder));

	if (EXC_FLAG_UNWIND & pExc->ExceptionFlags)
		wrk.UnwindLocalWithGuard();
	else
	{
		if (ExcMon::sPInst)
		{
			if (ExcMon::sPInst->mPExc != pExc)
				ExcMon::sPInst->AppendExc(pExc);

			wrk.Catch();

		}
		else
		{
			// probably a SEH exception
			ExcMon excMon(pExc, pCpuCtx);

			wrk.Catch();

			excMon.RaiseExcRaw(pExcRegCpp->mPPrev);
			excMon.NormalExit();
			return ExceptionContinueExecution;
		}
	}

	return ExceptionContinueSearch;
}

extern "C" EXCEPTION_DISPOSITION __cdecl __CxxFrameHandler3(int a, int b, int c, int d) {
	_asm {
		mov ecx, eax
		jmp FrameInfo::Handler
	}
}
extern "C" void __stdcall _CxxThrowException(void* pObj, _s__ThrowInfo const* pType) {
	if (pObj && pType)
	{
		EXCEPTION_RECORD exc;
		exc.ExceptionCode = 0xe06d7363;
		exc.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
		exc.ExceptionRecord = nullptr; // nested exc
		exc.NumberParameters = 3;
		exc.ExceptionAddress = _CxxThrowException;

		exc.ExceptionInformation[0] = 0x19930520;
		exc.ExceptionInformation[1] = ULONG_PTR(pObj);
		exc.ExceptionInformation[2] = ULONG_PTR(pType);

		Exc::RaiseExc(exc);

	}
	else
	{
		// rethrow
		Verify(nullptr != ExcMon::sPInst);
		ASSERT(ExcMon::s_pInst->m_pExc && ExcMon::s_pInst->m_pCpuCtx);

		WrapExcRecord wrk{};
		wrk.mPExc = ExcMon::sPInst->mPExc;
		wrk.mPCpuCtx = ExcMon::sPInst->mPCpuCtx;
		wrk.RaiseExcRaw(ExcReg::get_Top());
	}
}


__declspec (naked)
EXCEPTION_DISPOSITION __cdecl VCxxFrameHandler3(int a, int b, int c, int d)
{
	_asm {
		mov ecx, eax
		jmp FrameInfo::Handler
	};
}

void __stdcall VCxxThrowException(void* pObj, _s__ThrowInfo const* pType)
{
	if (pObj && pType)
	{
		EXCEPTION_RECORD exc;
		exc.ExceptionCode = 0xe06d7363;
		exc.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
		exc.ExceptionRecord = nullptr; // nested exc
		exc.NumberParameters = 3;
		exc.ExceptionAddress = _CxxThrowException;

		exc.ExceptionInformation[0] = 0x19930520;
		exc.ExceptionInformation[1] = ULONG_PTR(pObj);
		exc.ExceptionInformation[2] = ULONG_PTR(pType);

		Exc::RaiseExc(exc);

	}
	else
	{
		// rethrow
		Verify(nullptr != ExcMon::sPInst);
		ASSERT(ExcMon::s_pInst->m_pExc && ExcMon::s_pInst->m_pCpuCtx);

		WrapExcRecord wrk{};
		wrk.mPExc = ExcMon::sPInst->mPExc;
		wrk.mPCpuCtx = ExcMon::sPInst->mPCpuCtx;
		wrk.RaiseExcRaw(ExcReg::get_Top());
	}
}

bool __cdecl VUncaughtException()
{
	return nullptr != ExcMon::sPInst;
}

namespace Exc
{
#pragma pack(1)
	struct JumpOffset {
		UCHAR	mJumpCmd;
		DWORD	mDwOffset;
	};
#pragma pack()

	struct FunctionHack {
		bool		mBHacked;
		JumpOffset	mFuncBody;

		void Hack(const bool bSet, PVOID pfnOrg, PVOID pfnNew)
		{
			if (mBHacked != bSet)
			{
				DWORD dwProtectionOld = 0;
				VirtualProtect(pfnOrg, sizeof(mFuncBody), PAGE_READWRITE, &dwProtectionOld);

				if (bSet)
				{

					CopyMemory(&mFuncBody, pfnOrg, sizeof(mFuncBody));

					JumpOffset jump{};
					jump.mJumpCmd = 0xE9;
					jump.mDwOffset = DWORD(pfnNew) - DWORD(pfnOrg) - sizeof(jump);

					CopyMemory(pfnOrg, &jump, sizeof(jump));

				}
				else
					CopyMemory(pfnOrg, &mFuncBody, sizeof(mFuncBody));

				mBHacked = bSet;

				VirtualProtect(pfnOrg, sizeof(mFuncBody), dwProtectionOld, &dwProtectionOld);
			}
		}
	};

	void SetFrameHandler(const bool bSet)
	{
		static FunctionHack hack = { 0 };
		hack.Hack(bSet, __CxxFrameHandler3, VCxxFrameHandler3);
	}

	void SetThrowFunction(const bool bSet)
	{
		static FunctionHack hack = { 0 };
		hack.Hack(bSet, _CxxThrowException, VCxxThrowException);
	}

	void SetUncaughtExc(const bool bSet)
	{
		static FunctionHack hack = { 0 };
		hack.Hack(bSet, std::uncaught_exceptions, VUncaughtException);
	}

	void RaiseExc(EXCEPTION_RECORD& exc, CONTEXT* pCtx) noexcept {
		static CONTEXT ctx = { 0 };

		if (!pCtx)
			pCtx = &ctx;

		if (ExcMon::sPInst)
		{
			ExcMon::sPInst->AppendExc(&exc);
			ExcMon::sPInst->mPCpuCtx = pCtx;
			ExcMon::sPInst->RaiseExcRaw(ExcMon::sPInst);

		}
		else
		{
			ExcMon mon(&exc, pCtx);

			mon.RaiseExcRaw(&mon);
			mon.NormalExit();
		}
	}

	EXCEPTION_RECORD* GetCurrentExc()
	{
		return ExcMon::sPInst ? ExcMon::sPInst->mPExc : nullptr;
	}
}



#endif