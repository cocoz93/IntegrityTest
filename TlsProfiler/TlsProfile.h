//
#pragma once
#pragma comment(lib, "winmm")

#ifndef ____TLSPROFILE_H____
#define ____TLSPROFILE_H____

#include <windows.h>
#include <strsafe.h>

/** TLS 프로파일러 개념도`
``
`[ Thread 1 ]         [ Thread 2 ]         [ Thread 3 ]    ...`
`     |                   |                   |`<br/>
`     v                   v                   v`
`+----------------+  +----------------+  +----------------+`
`| TLS Index      |  | TLS Index      |  | TLS Index      |`
`|   pSample []   |  |   pSample []   |  |   pSample []   |`
`+----------------+  +----------------+  +----------------+`
`         \                 |                  /`
`          \                |                 /`
`           \               |                /`
`            v              v               v`
`+-------------------------------------------------------------+`
`|                 전역 배열: g_SaveProfile[]                  |`
`+-------------------------------------------------------------+`
`| [0] ThreadID: A | pSample: (Thread 1의 배열)                |`
`| [1] ThreadID: B | pSample: (Thread 2의 배열)                |`
`| [2] ThreadID: C | pSample: (Thread 3의 배열)                |`
`| ...             | ...                                       |`
`+-------------------------------------------------------------+`
*/

//config
// 스레드당 할당되는 TLS포인터 배열사이즈 크기
const int ProfilerSampleDataSize = 10; 

// 배열포인터를 저장하는 전역배열 크기 (출력용)
const int ProfilerglobalDataSize = 100; 

// Thread-Safe TLS Profiler
class CTlsProfiler
{
private:
	// 실제 프로파일링 될 샘플 데이터 (스레드당 n개씩)
	struct ProfileSample
	{
		ProfileSample() : UseFlag(false), BeginCalled(false), Name{ 0 }, StartTime{ 0 }, TotalTime(0),
			Min(1'000'000'000), Max(0), Call(0) {}

		bool	UseFlag;   							// 프로파일의 사용 여부. (배열시에만)
		bool	BeginCalled;
		WCHAR	Name[64];							// 프로파일 샘플 이름.

		LARGE_INTEGER StartTime;					// 프로파일 샘플 실행 시간.
		double	TotalTime;							// 전체 사용시간 카운터 Time. 
		double	Min;								// 최소 사용시간 카운터 Time. 
		double	Max;								// 최대 사용시간 카운터 Time. 
		unsigned __int64 Call;						// 누적 호출 횟수.
	};

	// TLS에 저장될 프로파일 데이터. 내 스레드에서만 접근하기때문에 동기화 X
	struct ProfilerThreadData
	{
		ProfileSample* pSample = nullptr;
		DWORD ThreadID = -1;
	};


	// 다른곳에서 사용금지
private:
	explicit CTlsProfiler();
	virtual ~CTlsProfiler();
	CTlsProfiler(const CTlsProfiler&) = delete;
	CTlsProfiler& operator=(const CTlsProfiler&) = delete;

public:
	static CTlsProfiler* GetProfiler()
	{
		static CTlsProfiler profiler;
		return &profiler;
	}

public:
	bool Begin(const WCHAR* SampleName);
	bool End(const WCHAR* SampleName);
	bool GetSample(const WCHAR* SampleName, ProfileSample** ppOutSample);
	bool SaveProfile();
	void DeleteAllTlsSamples();

private:
	DWORD _TlsIndex;
	double _MicroFreq;

	// 출력전용 전역데이터
	ProfilerThreadData g_SaveProfile[ProfilerglobalDataSize];
};

extern CTlsProfiler* g_profiler;
#define BEGIN_PROFILE(Name) g_profiler->Begin(Name)
#define END_PROFILE(Name)   g_profiler->End(Name)
#define	SAVE_PROFILE()		g_profiler->SaveProfile()

#endif

