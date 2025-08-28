
#include <iostream>
#include <atomic>
#include <thread>
#include "TlsProfile.h"

using namespace std;


void ProfileTest()
{
	unsigned long long val = 0;
	for (int i = 0; i < 100; ++i)
	{
		BEGIN_PROFILE(L"TestProfile");
		
		Sleep(100);
		END_PROFILE(L"TestProfile");
	}
}

int main()
{
	std::thread t[100];

	for (int i = 0; i < 100; ++i)
		t[i] = std::thread(ProfileTest);

	for (int i = 0; i < 100; ++i)
		t[i].join();

	SAVE_PROFILE();
	

	return 0;
}
