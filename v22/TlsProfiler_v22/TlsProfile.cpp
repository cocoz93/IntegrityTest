//
#include "TlsProfile.h"



CTlsProfiler* g_profiler = CTlsProfiler::GetProfiler();

CTlsProfiler::CTlsProfiler() : _TlsIndex(0), _MicroFreq(0)
{
	timeBeginPeriod(1);

	LARGE_INTEGER Freq;
	QueryPerformanceFrequency(&Freq);

	this->_MicroFreq = Freq.QuadPart / (double)1000000; //백만분의 1초
	this->_TlsIndex = TlsAlloc();

	// TlsIndex할당 실패
	if (_TlsIndex == TLS_OUT_OF_INDEXES)
	{
		// int* p = 0; *p = 0; // 크래시는 과하다
		// CriticalError Log
	}
}

CTlsProfiler::~CTlsProfiler()
{
	timeEndPeriod(1);
	DeleteAllTlsSamples();
	TlsFree(this->_TlsIndex);
}


#pragma optimize("", off)
bool CTlsProfiler::Begin(const WCHAR* SampleName)
{
	ProfileSample* pSample = nullptr;
	GetSample(SampleName, &pSample);

	if (nullptr == pSample)
		return false;

	if (true == pSample->BeginCalled)
		return false;

	pSample->BeginCalled = true;
	if (false == QueryPerformanceCounter(&pSample->StartTime))
		return false;

	return true;
}

bool CTlsProfiler::End(const WCHAR* SampleName)
{
	LARGE_INTEGER EndTime, ResTime;
	if (false == QueryPerformanceCounter(&EndTime))
		return false;

	ProfileSample* pSample = nullptr;
	GetSample(SampleName, &pSample);

	if (nullptr == pSample)
		return false;

	// Begin없이 End호출되었거나 End만 두번 호출되면 return
	if (false == pSample->BeginCalled)
		return false;
	else
		pSample->BeginCalled = false;

	ResTime.QuadPart = EndTime.QuadPart - pSample->StartTime.QuadPart;
	double elapsed = (double)ResTime.QuadPart;

	// 최대값 갱신
	if (pSample->Call == 0 || pSample->Max < elapsed)
		pSample->Max = elapsed;

	// 최소값 갱신
	if (pSample->Call == 0 || pSample->Min > elapsed)
		pSample->Min = elapsed;

	pSample->TotalTime += ((double)ResTime.QuadPart);
	++pSample->Call;

	pSample->StartTime.QuadPart = 0;

	return true;
}

bool CTlsProfiler::GetSample(const WCHAR* SampleName, ProfileSample** ppOutSample)
{
	ProfileSample* pSample = (ProfileSample*)TlsGetValue(this->_TlsIndex);

	// Tls가 최초 호출된경우(세팅되지않은 경우)
	if (nullptr == pSample)
	{
		// ProfileSample 동적할당
		pSample = new ProfileSample[ProfilerSampleDataSize];

		// TLS값 세팅
		if (false == TlsSetValue(this->_TlsIndex, pSample))
		{
			// CriticalError Log
			// DWORD err = GetLastError();
			return false;
		}

		// 처음 Tls가 할당될때
		// 원하는 스레드에서 출력을 위한 전역배열에 따로 저장한다.
		for (int i = 0; i < ProfilerglobalDataSize; ++i)
		{
			if ((g_SaveProfile[i].ThreadID == -1)
				&& g_SaveProfile[i].pSample == nullptr)
			{
				g_SaveProfile[i].ThreadID = GetCurrentThreadId();
				g_SaveProfile[i].pSample = pSample;
				break;
			}
		}
	}


	//Tls가 이미 할당된 경우
	int i;
	for (i = 0; i < ProfilerSampleDataSize; ++i)
	{
		// TLS가 이미 할당된 경우 샘플중에 원하는 이름의 것을 뺴서 리턴.
		if ((true == pSample[i].UseFlag) && (0 == wcscmp(pSample[i].Name, SampleName)))
		{
			*ppOutSample = &pSample[i];
			break;
		}

		//샘플이 없는경우 샘플을 새로 만듬
		if (0 == wcscmp(pSample[i].Name, L""))
		{
			// 샘플 초기화
			StringCchPrintf(pSample[i].Name,
				sizeof(pSample[i].Name) / sizeof(WCHAR),
				L"%s",
				SampleName);

			pSample[i].UseFlag = true; 
			// 나머지는 생성자에서 초기화

			// Out Param
			*ppOutSample = &pSample[i];
			break;
		}
	}
	return true;
}
#pragma optimize("", on)

bool CTlsProfiler::SaveProfile()
{
	SYSTEMTIME NowTime;
	WCHAR fileName[128];
	WCHAR Buff[256] = { L"﻿ ThreadID |                Name  |           Average  |            Min   |            Max   |          Call |" };
	FILE* fp = NULL;

	// 파일 생성
	GetLocalTime(&NowTime);
	StringCchPrintf(fileName,
		sizeof(fileName),
		L"[Profiling Data] %4d-%02d-%02d %02d.%02d.txt",
		NowTime.wYear,
		NowTime.wMonth,
		NowTime.wDay,
		NowTime.wHour,
		NowTime.wMinute
	);

	// 파일 만들기
	errno_t err = _wfopen_s(&fp, fileName, L"wt, ccs=UNICODE");

	if (0 != err || NULL == fp)
	{
		wprintf(L"FileOpen fail");
		//CriticalError log
		return false;
	}

	fwprintf(fp, L"ThreadId |                Name  |           Average  |            Min   |            Max   |          Call |\n");
	fwprintf(fp, L"------------------------------------------------------------------------------------------------------------\n");


	//전역에 있는 SaveProfile배열중 TagName을 찾아 파일에 저장
	for (int i = 0; i < ProfilerglobalDataSize; ++i)
	{
		//더이상 저장할 데이터가 없다면 break..
		if (-1 == g_SaveProfile[i].ThreadID)
			break;

		//스레드 내에 샘플을 돌면서 파일에 저장
		for (int SampleCnt = 0; SampleCnt < ProfilerSampleDataSize; SampleCnt++)
		{
			//더이상 저장할 샘플이 없다면 break. 다음 데이터로 넘어감
			if (false == g_SaveProfile[i].pSample[SampleCnt].UseFlag)
				break;

			StringCchPrintf(Buff,
				sizeof(Buff) / sizeof(WCHAR),
				L"%8d | %20s | %16.4f㎲ | %14.4f㎲ | %14.4f㎲ | %13d |\n",
				g_SaveProfile[i].ThreadID,
				g_SaveProfile[i].pSample[SampleCnt].Name,
				g_SaveProfile[i].pSample[SampleCnt].TotalTime
				/ g_SaveProfile[i].pSample[SampleCnt].Call / this->_MicroFreq,
				g_SaveProfile[i].pSample[SampleCnt].Min / this->_MicroFreq,
				g_SaveProfile[i].pSample[SampleCnt].Max / this->_MicroFreq,
				g_SaveProfile[i].pSample[SampleCnt].Call
			);
			fwprintf(fp, Buff);

		}
		fwprintf(fp, L"------------------------------------------------------------------------------------------------------------\n");
	}

	fclose(fp);
	return true;
}

void CTlsProfiler::DeleteAllTlsSamples()
{
	for (int i = 0; i < ProfilerglobalDataSize; ++i)
	{
		if (g_SaveProfile[i].pSample != nullptr)
		{
			delete[] g_SaveProfile[i].pSample;
			g_SaveProfile[i].pSample = nullptr;
			g_SaveProfile[i].ThreadID = -1;
		}
	}
}


