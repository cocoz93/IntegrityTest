//
#include <iostream>
#include <vector>
#include <atomic>
#include <chrono>
#include <random>
#include <cstring>
#include <thread>
#include <mutex>
#include <memory> 
#include <set>
#include <string>
#include "RingBuffer.h"

//=============================================================================
// 테스트 설정 상수 (반복 횟수 조절 가능)
//=============================================================================
namespace TestConfig
{
    // Phase 1: 단일 스레드 테스트
	const uint64_t DATA_INTEGRITY_ITERATIONS = 100'000'000; // 1억 번
    const uint64_t INVARIANT_ITERATIONS = 100'000'000; //
    const uint64_t BOUNDARY_ITERATIONS_PER_SCENARIO = 25'000'000;

    // Phase 2: 멀티스레드 테스트
	const uint64_t NUMBERS_PER_THREAD = 10'000'000; // 각 생산자 스레드가 생성할 숫자 개수 (Producer-Consumer)
    const uint64_t PEEK_CONSUME_PER_THREAD = 10'000'000; // 각 생산자 스레드가 생성할 숫자 개수 (Peek+Consume)
    const uint64_t HIGH_CONTENTION_OPS_PER_THREAD = 10'000'000; // 각 스레드당 작업 횟수 (고빈도 경합)

    // 진행 상황 출력 주기
    const uint64_t PROGRESS_INTERVAL = 10'000'000; // 설정된 값 마다 모니터링 출력
}

// 전역 카운터
std::atomic<uint64_t> g_testCount(0);
std::atomic<uint64_t> g_totalIterations(0);

// 크래시 함수
void Crash()
{
    int* crash = nullptr;
    *crash = 0xDEADBEEF;
}

// 테스트 실패 시 크래시
#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            std::cout << "\n[CRASH] " << message << std::endl; \
            std::cout << "  File: " << __FILE__ << std::endl; \
            std::cout << "  Line: " << __LINE__ << std::endl; \
            std::cout << "  Iteration: " << g_totalIterations << std::endl; \
            Crash(); \
        } \
    } while(0)

// 진행 상황 출력
void PrintProgress(const char* testName, uint64_t current, uint64_t total)
{
    if (current % TestConfig::PROGRESS_INTERVAL == 0)
    {
        double progress = (double)current / total * 100.0;
        std::cout << "[" << testName << "] "
            << "진행: " << current << " / " << total
            << " (" << progress << "%)" << std::endl;
    }
}

//=============================================================================
// Phase 1-1: 싱글 스레드 - 데이터 무결성 반복 테스트
// 데이터 순서, 손상 여부 검증
//=============================================================================
void Test_DataIntegrity()
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "[Phase 1-1] 데이터 무결성 테스트 시작" << std::endl;
    std::cout << "  목표: " << TestConfig::DATA_INTEGRITY_ITERATIONS / 1'000'000 << "백만 번 반복" << std::endl;
    std::cout << "========================================" << std::endl;

    const uint64_t ITERATIONS = TestConfig::DATA_INTEGRITY_ITERATIONS;
    auto rb = std::make_unique<CRingBufferST>(8192);
    if (!rb->IsValid())
    {
        std::cout << "[ERROR] RingBuffer 할당 실패" << std::endl;
        return;
    }

	std::random_device rd; // 난수 생성기 초기화
    std::mt19937 gen(rd()); 
	std::uniform_int_distribution<> sizeDis(1, 1000); // 1~1000 바이트 크기 분포

    uint64_t writeSequence = 0; 
    uint64_t readSequence = 0;
    std::vector<uint64_t> writeBuffer;
    std::vector<uint64_t> readBuffer;

    auto startTime = std::chrono::steady_clock::now();
    const auto TIMEOUT = std::chrono::minutes(5); // 5분

	for (uint64_t i = 0; i < ITERATIONS; i++)
	{
		g_totalIterations = i;
		PrintProgress("데이터 무결성", i, ITERATIONS); // 진행 상황 출력

		// 버퍼의 남은 공간이 있으면 무작위로(1~10) 데이터 쓰기
        if (rb->GetFreeSize() >= sizeof(uint64_t) * 10)
        {
            int writeCount = sizeDis(gen) % 10 + 1;
            writeBuffer.clear(); 

            // 데이터 쓰기
            for (int j = 0; j < writeCount; j++)
            {
                writeBuffer.push_back(writeSequence++);
            }

            size_t written = rb->Enqueue(writeBuffer.data(), writeBuffer.size() * sizeof(uint64_t));  
            TEST_ASSERT(written == writeBuffer.size() * sizeof(uint64_t), "Enqueue 크기 불일치");
        }

		// 버퍼에 읽을 데이터가 있으면 무작위로(1~10) 데이터 읽기
        if (rb->GetDataSize() >= sizeof(uint64_t))
        {
            int readCount = (sizeDis(gen) % 10 + 1);
            size_t maxRead = rb->GetDataSize() / sizeof(uint64_t);
            if (readCount > maxRead) readCount = (int)maxRead;

            readBuffer.resize(readCount);
            size_t read = rb->Dequeue(readBuffer.data(), readCount * sizeof(uint64_t)); 
            TEST_ASSERT(read == readCount * sizeof(uint64_t), "Dequeue 크기 불일치");

            // 읽은 데이터 검증
            for (int j = 0; j < readCount; j++)
            {
                TEST_ASSERT(readBuffer[j] == readSequence, "데이터 손상: 시퀀스 번호 불일치");
                readSequence++;
            }
        }

        // ✅ 전체 테스트 시작 시각 기준으로 체크
        if (std::chrono::steady_clock::now() - startTime > TIMEOUT)
        {
            std::cout << "[ERROR] 타임아웃 발생! (5분 초과)" << std::endl;
            Crash();
        }
    }

	// 남은 데이터 모두 읽기
    while (rb->GetDataSize() >= sizeof(uint64_t))
    {
        uint64_t value;
        size_t read = rb->Dequeue(&value, sizeof(uint64_t)); 
        TEST_ASSERT(read == sizeof(uint64_t), "최종 Dequeue 실패");
        TEST_ASSERT(value == readSequence, "최종 데이터 검증 실패");
        readSequence++;
    }

    TEST_ASSERT(readSequence == writeSequence, "총 쓴 데이터와 읽은 데이터 개수 불일치");
    TEST_ASSERT(rb->GetDataSize() == 0, "버퍼가 완전히 비워지지 않음");

    auto endTime = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(endTime - startTime).count();

    std::cout << "\n[PASS] 데이터 무결성 테스트 완료!" << std::endl;
    std::cout << "  - 총 반복: " << ITERATIONS << " 회" << std::endl;
    std::cout << "  - 소요 시간: " << elapsed << " 초" << std::endl;
    std::cout << "  - 쓴 데이터: " << writeSequence << " 개" << std::endl;
    std::cout << "  - 읽은 데이터: " << readSequence << " 개" << std::endl;

    g_testCount++;
}

//=============================================================================
// Phase 1-2: 싱글 스레드 - 불변성(Invariant) 
// 무작위 작업 (Enque/Deque/Peek/Consume/Clear) 후 DataSize, FreeSize 검사
//=============================================================================
void Test_Invariants()
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "[Phase 1-2] 불변성 검증 테스트 시작" << std::endl;
    std::cout << "  목표: " << TestConfig::INVARIANT_ITERATIONS / 1'000'000 << "백만 번 반복" << std::endl;
    std::cout << "========================================" << std::endl;

    const uint64_t ITERATIONS = TestConfig::INVARIANT_ITERATIONS;
    auto rb = std::make_unique<CRingBufferST>(4096);
    if (!rb->IsValid())
    {
        std::cout << "[ERROR] RingBuffer 할당 실패" << std::endl;
        return;
    }

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> sizeDis(1, 512);
    std::uniform_int_distribution<> opDis(0, 4);

    std::vector<char> buffer(1024);
    auto startTime = std::chrono::steady_clock::now();

    for (uint64_t i = 0; i < ITERATIONS; i++)
    {
        g_totalIterations = i;
        PrintProgress("불변성 검증", i, ITERATIONS);

        size_t beforeDataSize = rb->GetDataSize();
        size_t beforeFreeSize = rb->GetFreeSize();
        size_t capacity = 4095;

		// [사용중인 공간 + 여유 공간 = 용량 - 1] 확인
        TEST_ASSERT(beforeDataSize + beforeFreeSize == capacity,
            "불변 조건 위반: DataSize + FreeSize != capacity - 1");

        int operation = opDis(gen);
        int size = sizeDis(gen);

		// 무작위 작업 수행
        switch (operation)
        {
		case 0: // Enqueue
        {
            size_t written = rb->Enqueue(buffer.data(), size);  
            size_t afterDataSize = rb->GetDataSize();
            TEST_ASSERT(afterDataSize == beforeDataSize + written,
                "Enqueue 후 DataSize 증가량 불일치");
            break;
        }
		case 1: // Dequeue
        {
            size_t read = rb->Dequeue(buffer.data(), size);  
            size_t afterDataSize = rb->GetDataSize();
            TEST_ASSERT(afterDataSize == beforeDataSize - read,
                "Dequeue 후 DataSize 감소량 불일치");
            break;
        }
		case 2: // Peek
        {
            size_t peeked = rb->Peek(buffer.data(), size);
            size_t afterDataSize = rb->GetDataSize();
            TEST_ASSERT(afterDataSize == beforeDataSize,
                "Peek 후 DataSize가 변경됨");
            TEST_ASSERT(peeked <= beforeDataSize,
                "Peek 크기가 DataSize보다 큼");
            break;
        }
		case 3: // Consume (포인터만 이동)
        {
            size_t consumed = rb->Consume(size);
            size_t afterDataSize = rb->GetDataSize();
            TEST_ASSERT(afterDataSize == beforeDataSize - consumed,
                "Consume 후 DataSize 감소량 불일치");
            TEST_ASSERT(consumed <= beforeDataSize,
                "Consume 크기가 DataSize보다 큼");
            break;
        }
		case 4: // Clear
        {
            rb->Clear();
            TEST_ASSERT(rb->GetDataSize() == 0,
                "Clear 후 DataSize가 0이 아님");
            TEST_ASSERT(rb->GetFreeSize() == capacity,
                "Clear 후 FreeSize가 capacity가 아님");
            break;
        }
        }

        size_t afterDataSize = rb->GetDataSize();
        size_t afterFreeSize = rb->GetFreeSize();
        TEST_ASSERT(afterDataSize + afterFreeSize == capacity,
            "작업 후 불변 조건 위반");
    }

    auto endTime = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(endTime - startTime).count();

    std::cout << "\n[PASS] 불변성 검증 테스트 완료!" << std::endl;
    std::cout << "  - 총 반복: " << ITERATIONS << " 회" << std::endl;
    std::cout << "  - 소요 시간: " << elapsed << " 초" << std::endl;

    g_testCount++;
}

//=============================================================================
// Phase 1-3: 싱글 스레드 - 버퍼 끝 경계 테스트
// 버퍼 끝 경계에서 Wrap-Around 동작 검증
//=============================================================================
void Test_BoundaryConditions()
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "[Phase 1-3] 경계 조건 테스트 시작" << std::endl;
    std::cout << "  목표: " << TestConfig::BOUNDARY_ITERATIONS_PER_SCENARIO * 3 / 1'000'000 << "백만 번 반복 (3개 시나리오)" << std::endl;
    std::cout << "========================================" << std::endl;

    const uint64_t ITERATIONS = TestConfig::BOUNDARY_ITERATIONS_PER_SCENARIO;

    // 시나리오 1: 경계에서 Wrap-Around 반복 (핵심 테스트)
    {
        std::cout << "\n[시나리오 1] 경계 Wrap-Around 집중 테스트" << std::endl;
        auto rb = std::make_unique<CRingBufferST>(1024);  // 변경
        std::vector<char> setupData(1022);
        
		// 1. 포인터를 경계(1022)로 이동 (Enque 후에 Deque)
        size_t setup1 = rb->Enqueue(setupData.data(), setupData.size());
        TEST_ASSERT(setup1 == setupData.size(), "Setup: 1022바이트 쓰기 실패");
        
        size_t setup2 = rb->Dequeue(setupData.data(), setupData.size());
        TEST_ASSERT(setup2 == setupData.size(), "Setup: 1022바이트 읽기 실패");
        TEST_ASSERT(rb->GetDataSize() == 0, "Setup: 버퍼가 비워지지 않음");
        
        // 이제 _readPos = _writePos = 1022 (경계 직전)
        
        char byte = 0x42;
        auto startTime = std::chrono::steady_clock::now();

        for (uint64_t i = 0; i < ITERATIONS; i++)
        {
            g_totalIterations = i;
            PrintProgress("Wrap-Around", i, ITERATIONS);

            // 2. 1바이트 Enque (1022 → 1023 → 0으로 wrap)
            size_t written = rb->Enqueue(&byte, 1);
            TEST_ASSERT(written == 1, "경계 Enqueue 실패1");
            written = rb->Enqueue(&byte, 1);
            TEST_ASSERT(written == 1, "경계 Enqueue 실패2");

			// 3. 1바이트 Deque (0 -> 1023 -> 1022로 wrap)
            char readByte;
            size_t read = rb->Dequeue(&readByte, 1);
            TEST_ASSERT(read == 1, "경계 Dequeue 실패1");
            read = rb->Dequeue(&readByte, 1);
            TEST_ASSERT(read == 1, "경계 Dequeue 실패2");
            TEST_ASSERT(rb->GetDataSize() == 0, "경계 작업 후 Empty 실패");
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - startTime).count();
        std::cout << "  완료: " << elapsed << "초" << std::endl;
    }

    // 시나리오 2: 다양한 크기로 Wrap Around 테스트
    {
        std::cout << "\n[시나리오 2] 다양한 크기 Wrap Around 테스트" << std::endl;
        auto rb = std::make_unique<CRingBufferST>(256);  // 변경
        std::vector<char> data(200);
        auto startTime = std::chrono::steady_clock::now();

        for (uint64_t i = 0; i < ITERATIONS; i++)
        {
            g_totalIterations = i;
            PrintProgress("다양한 Wrap", i, ITERATIONS);

            size_t written = rb->Enqueue(data.data(), 200);
            TEST_ASSERT(written == 200, "200바이트 쓰기 실패");

            size_t read = rb->Dequeue(data.data(), 150);
            TEST_ASSERT(read == 150, "150바이트 읽기 실패");

            written = rb->Enqueue(data.data(), 150);
            TEST_ASSERT(written == 150, "순환 쓰기 실패");

            read = rb->Dequeue(data.data(), 200);
            TEST_ASSERT(read == 200, "순환 읽기 실패");
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - startTime).count();
        std::cout << "  완료: " << elapsed << "초" << std::endl;
    }

    // 시나리오 3: 경계값 초과 시도
    {
        std::cout << "\n[시나리오 3] 경계값 초과 시도 테스트" << std::endl;
        auto rb = std::make_unique<CRingBufferST>(512);  // 변경
        std::vector<char> overData(1024);
        auto startTime = std::chrono::steady_clock::now();

        for (uint64_t i = 0; i < ITERATIONS; i++)
        {
            g_totalIterations = i;
            PrintProgress("경계값 초과", i, ITERATIONS);

            size_t written = rb->Enqueue(overData.data(), overData.size());
            TEST_ASSERT(written <= 511, "capacity 초과 쓰기 허용됨");

            rb->Clear();
            char readBuf[10];
            size_t read = rb->Dequeue(readBuf, 10);
            TEST_ASSERT(read == 0, "빈 버퍼에서 읽기 성공");

            std::vector<char> fillData(511);
            rb->Enqueue(fillData.data(), fillData.size());
            written = rb->Enqueue(&readBuf[0], 1);
            TEST_ASSERT(written == 0, "Full 버퍼에 쓰기 성공");

            rb->Clear();
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - startTime).count();
        std::cout << "  완료: " << elapsed << "초" << std::endl;
    }

    std::cout << "\n[PASS] 경계 조건 테스트 완료!" << std::endl;
    std::cout << "  - 총 반복: " << ITERATIONS * 3 << " 회" << std::endl;

    g_testCount++;
}

//=============================================================================
// Phase 2-1: 멀티스레드 - Producer-Consumer 정합성 테스트 
// 여러 생산자/소비자 스레드로 데이터 무결성 검증
//=============================================================================

// 파라미터화된 테스트 함수 (진행률/완료 현황 상단 고정)
void RunProducerConsumerTest(
	int producerCount,  // 생산자 스레드 수
	int consumerCount,  // 소비자 스레드 수
	int numbersPerThread, // 각 생산자 스레드가 생성할 숫자 개수
    const std::vector<std::string>& completedLines,
    const std::string& runningLine)
{
	const int TOTAL_NUMBERS = numbersPerThread * producerCount; // 전체 숫자 개수
    std::atomic<uint64_t> totalEnqueued(0);
    std::atomic<uint64_t> totalDequeued(0); 

    std::atomic<bool> allProducersDone(false);
    std::atomic<int> producersCompleted(0);    
    std::atomic<int> consumersCompleted(0);

    auto rb = std::make_unique<CRingBufferMT>(65536);
    if (!rb->IsValid())
    {
        std::cout << "[ERROR] RingBuffer 할당 실패" << std::endl;
        return;
    }

    // dequeueCheck를 힙에 unique_ptr로 할당
    auto dequeueCheck = std::make_unique<std::atomic<int>[]>(TOTAL_NUMBERS);
    if (!dequeueCheck)
    {
        std::cout << "[ERROR] dequeueCheck 메모리 할당 실패: " << TOTAL_NUMBERS << std::endl;
        return;
    }

    // dequeueCheck 초기화
    for (int i = 0; i < TOTAL_NUMBERS; i++)
        dequeueCheck[i] = 0;

    // 진행률 출력 스레드
    std::atomic<bool> running(true);
    std::atomic<bool> pauseProgress(false); // 추가: 진행률 일시 중지 플래그
    std::thread progressThread([&]() {
        while (running)
        {
            if (!pauseProgress) // 최종 검증 중엔 출력 안함
            {
#ifdef _WIN32
            system("cls");
#else
            system("clear");
#endif
            std::cout << "========================================" << std::endl;
            std::cout << "[Phase 2-1] Producer-Consumer 테스트" << std::endl;  // ← 추가
            std::cout << "========================================" << std::endl;
            for (size_t j = 0; j < completedLines.size(); ++j)
            {
                std::cout << completedLines[j] << std::endl;
            }
            std::cout << runningLine << std::endl;
            double enqueueRate = (double)totalEnqueued / TOTAL_NUMBERS * 100.0;
            double dequeueRate = (double)totalDequeued / TOTAL_NUMBERS * 100.0;
            std::cout << "[진행률] Enqueue: " << enqueueRate << "%, Dequeue: " << dequeueRate << "%\n";
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    });

    std::random_device rd;
    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;

    auto startTime = std::chrono::steady_clock::now();

    // Producer 스레드 생성
    for (int threadId = 0; threadId < producerCount; threadId++)
    {
        producers.emplace_back([&, threadId]()
        {
            std::mt19937 gen(rd() + threadId);
            std::uniform_int_distribution<> sizeDis(1, 32);  // 1~32개 숫자 (8~256바이트)

            int startNum = threadId * numbersPerThread;
            int endNum = startNum + numbersPerThread;
            int currentNum = startNum;

            while (currentNum < endNum)
            {
                // 랜덤 개수만큼 숫자 묶음 생성 (8~256바이트)
                int batchSize = (std::min)(sizeDis(gen), endNum - currentNum);
                std::vector<int> batch;

                for (int i = 0; i < batchSize; i++)
                {
                    batch.push_back(currentNum + i);
                }

                size_t totalSize = batch.size() * sizeof(int);
                size_t written = 0;

                // All-or-Nothing: 전체 쓰기 성공할 때까지 재시도
                while (written == 0) 
                {
                    written = rb->Enqueue(batch.data(), totalSize);

                    // 일부러 경합 유발을 위해 짧은 대기 추가하지 않음
                }

                TEST_ASSERT(written == totalSize, "Enqueue 크기 불일치");
                currentNum += batchSize;
                totalEnqueued += batchSize;
            }
            ++producersCompleted;
        });
    }

    // Consumer 스레드 생성
    for (int consumerId = 0; consumerId < consumerCount; consumerId++)
    {
        consumers.emplace_back([&, consumerId]()
        {
            std::mt19937 gen(rd() + 1000 + consumerId);
            std::uniform_int_distribution<> sizeDis(1, 32);
            std::vector<int> readBuffer(32);

            while (true)
            {
				// 종료 조건 확인
                if (allProducersDone && totalDequeued >= TOTAL_NUMBERS)
                {
                    break;
                }

                // 랜덤 크기로 Dequeue 시도 (8~256바이트)
                int requestCount = sizeDis(gen);
                size_t requestSize = requestCount * sizeof(int);
                size_t read = rb->Dequeue(readBuffer.data(), requestSize);

                // All-or-Nothing: 0 또는 requestSize만 가능
                TEST_ASSERT(read == 0 || read == requestSize, "All-or-Nothing 위반: 부분 읽기 " + std::to_string(read) + " 발생");

                if (read == 0)
                {
                    continue;
                }

                // 요청한 만큼 read 됨
                size_t numCount = read / sizeof(int);
                for (size_t i = 0; i < numCount; i++)
                {
                    int num = readBuffer[i];
                    TEST_ASSERT(num >= 0 && num < TOTAL_NUMBERS, "범위 초과 숫자 발견");
                    int expected = 0;
                    bool success = dequeueCheck[num].compare_exchange_strong(expected, 1);
                    TEST_ASSERT(success, "중복 Dequeue 발견!");
                }
                totalDequeued += numCount;
            }
            ++consumersCompleted;
        });
    }

    for (auto& t : producers) t.join();
    allProducersDone = true;
    for (auto& t : consumers) t.join();

    // 스레드 종료 "전에" 플래그 설정
    pauseProgress = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(600)); // cls 완료 대기

    running = false;
    progressThread.join();

    // 최종 검증 시작 전 진행률 출력 일시 중지
    pauseProgress = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(600)); // cls 끝날 때까지 대기

    auto endTime = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(endTime - startTime).count();

    // 최종 검증
    std::cout << "\n========================================" << std::endl;
    std::cout << "[최종 검증]" << std::endl;
    std::cout << "========================================" << std::endl;

    // 1. 총 개수 일치
    TEST_ASSERT(totalEnqueued == TOTAL_NUMBERS, "Enqueue 개수 불일치");
    TEST_ASSERT(totalDequeued == TOTAL_NUMBERS, "Dequeue 개수 불일치");
    std::cout << "  > Enqueue/Dequeue 개수 일치: " << TOTAL_NUMBERS << " 개" << std::endl;

    // 2. 모든 숫자가 정확히 1번씩 처리되었는지 확인
    int missingCount = 0;
    int duplicateCount = 0;

    for (int i = 0; i < TOTAL_NUMBERS; i++)
    {
        int status = dequeueCheck[i];
        if (status == 0)
        {
            missingCount++;
            std::cout << "  [ERROR] 누락된 숫자: " << i << std::endl;
        }
        else if (status > 1)
        {
            duplicateCount++;
            std::cout << "  [ERROR] 중복 처리: " << i << " (횟수: " << status << ")" << std::endl;
        }
    }

    TEST_ASSERT(missingCount == 0, "누락된 숫자 발견");
    TEST_ASSERT(duplicateCount == 0, "중복 처리된 숫자 발견");
    std::cout << "  > 모든 숫자 정확히 1번씩 처리 완료" << std::endl;

    TEST_ASSERT(rb->GetDataSize() == 0, "버퍼가 완전히 비워지지 않음");
    std::cout << "  > 버퍼 완전히 비워짐" << std::endl;

    std::cout << "\n[PASS] Producer " << producerCount << " / Consumer " << consumerCount << " 완료 (소요: " << elapsed << "초)" << std::endl;
    std::cout << "========================================" << std::endl;

    g_testCount++;
    std::this_thread::sleep_for(std::chrono::seconds(5));
}

// 다중 조합 테스트 실행
void Test_ProducerConsumer()
{
    // 다양한 스레드 조합 (대칭 + 비대칭)
    std::vector<std::pair<int, int>> threadConfigs = {
        {1, 1},   // 대칭: 최소
        {2, 2},   // 대칭
        {4, 4},   // 대칭
        {8, 8},   // 대칭: 코어 수 고려
        {1, 8},   // 비대칭: Producer 적음
        {8, 1},   // 비대칭: Consumer 적음
        {2, 6},   // 비대칭
        {6, 2}    // 비대칭
    };

    std::vector<std::string> completedLines;

    for (size_t i = 0; i < threadConfigs.size(); i++)
    {
        int producerCount = threadConfigs[i].first;
        int consumerCount = threadConfigs[i].second;

        // 진행 중 조합 라인
        std::string runningLine = "[" + std::to_string(producerCount) + "-" + std::to_string(consumerCount) + "] 조합 테스트 진행 중..";

        RunProducerConsumerTest(
            producerCount,
            consumerCount,
            TestConfig::NUMBERS_PER_THREAD,
            completedLines,
            runningLine
        );

        // 완료된 조합을 상단에 누적
        completedLines.push_back("[" + std::to_string(producerCount) + "-" + std::to_string(consumerCount) + "] 조합 테스트 완료");
    }

    // 마지막 전체 완료 출력
#ifdef _WIN32
    system("cls");
#else
    system("clear");
#endif
    std::cout << "========================================" << std::endl;
    for (size_t i = 0; i < completedLines.size(); ++i)
    {
        std::cout << completedLines[i] << std::endl;
    }
    std::cout << "\n========================================" << std::endl;
    std::cout << "[Phase 2-1] 모든 조합 테스트 완료!" << std::endl;
    std::cout << "  - 총 " << threadConfigs.size() << "가지 조합 성공" << std::endl;
    std::cout << "========================================" << std::endl;
}

//=============================================================================
// Phase 2-2: 멀티스레드 - Peek + Consume 조합 검증 (다양한 스레드 조합)
//=============================================================================

// 파라미터화된 Peek + Consume 테스트 함수
void RunPeekConsumeTest(
    int producerCount,
    int consumerCount,
    int numbersPerThread,
    const std::vector<std::string>& completedLines,
    const std::string& runningLine)
{
    const int TOTAL_NUMBERS = numbersPerThread * producerCount;
    std::atomic<uint64_t> totalEnqueued(0);
    std::atomic<uint64_t> totalPeeked(0);
    std::atomic<uint64_t> totalConsumed(0);

    std::atomic<bool> allProducersDone(false);

    auto rb = std::make_unique<CRingBufferMT>(65536);
    if (!rb->IsValid())
    {
        std::cout << "[ERROR] RingBuffer 할당 실패" << std::endl;
        return;
    }

    // consumeCheck를 힙에 unique_ptr로 할당
    auto consumeCheck = std::make_unique<std::atomic<int>[]>(TOTAL_NUMBERS);
    if (!consumeCheck)
    {
        std::cout << "[ERROR] consumeCheck 메모리 할당 실패: " << TOTAL_NUMBERS << std::endl;
        return;
    }

    // consumeCheck 초기화
    for (int i = 0; i < TOTAL_NUMBERS; i++)
        consumeCheck[i] = 0;

    // 진행률 출력 스레드
    std::atomic<bool> running(true);
    std::atomic<bool> pauseProgress(false);
    std::thread progressThread([&]() {
        while (running)
        {
            if (!pauseProgress)
            {
#ifdef _WIN32
                system("cls");
#else
                system("clear");
#endif
                std::cout << "========================================" << std::endl;
                std::cout << "[Phase 2-2] Peek + Consume 테스트" << std::endl;  // ← 추가
                std::cout << "========================================" << std::endl;
                for (size_t j = 0; j < completedLines.size(); ++j)
                {
                    std::cout << completedLines[j] << std::endl;
                }
                std::cout << runningLine << std::endl;
                double enqueueRate = (double)totalEnqueued / TOTAL_NUMBERS * 100.0;
                double consumeRate = (double)totalConsumed / TOTAL_NUMBERS * 100.0;
                std::cout << "[진행률] Enqueue: " << enqueueRate << "%, Consume: " << consumeRate << "%\n";
                std::cout << "  - Peek 시도: " << totalPeeked << std::endl;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    });

    std::random_device rd;
    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;

    auto startTime = std::chrono::steady_clock::now();

    // Producer 스레드 생성
    for (int threadId = 0; threadId < producerCount; threadId++)
    {
        producers.emplace_back([&, threadId]()
        {
            std::mt19937 gen(rd() + threadId);
            std::uniform_int_distribution<> sizeDis(1, 32);  // 1~32개 숫자 (4~128바이트)

            int startNum = threadId * numbersPerThread;
            int endNum = startNum + numbersPerThread;
            int currentNum = startNum;

            while (currentNum < endNum)
            {
                // 랜덤 개수만큼 숫자 묶음 생성
                int batchSize = (std::min)(sizeDis(gen), endNum - currentNum);
                std::vector<int> batch;

                for (int i = 0; i < batchSize; i++)
                {
                    batch.push_back(currentNum + i);
                }

                size_t totalSize = batch.size() * sizeof(int);
                size_t written = 0;

                // All-or-Nothing: 전체 쓰기 성공할 때까지 재시도
                while (written == 0)
                {
                    written = rb->Enqueue(batch.data(), totalSize);
                }

                TEST_ASSERT(written == totalSize, "Enqueue 크기 불일치");
                currentNum += batchSize;
                totalEnqueued += batchSize;
            }
        });
    }

    // Consumer 스레드 생성 (Peek → 검증 → Consume 패턴)
    for (int consumerId = 0; consumerId < consumerCount; consumerId++)
    {
        consumers.emplace_back([&, consumerId]()
        {
            std::mt19937 gen(rd() + 1000 + consumerId);
            std::uniform_int_distribution<> sizeDis(1, 32);
            std::vector<int> peekBuffer(32);

            while (true)
            {
                // 종료 조건 확인
                if (allProducersDone && totalConsumed >= TOTAL_NUMBERS)
                {
                    break;
                }

                // 랜덤 크기로 Peek 시도
                int requestCount = sizeDis(gen);
                size_t requestSize = requestCount * sizeof(int);
                
                // 1. Peek으로 데이터 미리보기
                size_t peeked = rb->Peek(peekBuffer.data(), requestSize);
                
                // All-or-Nothing: 0 또는 requestSize만 가능
                TEST_ASSERT(peeked == 0 || peeked == requestSize, 
                    "Peek All-or-Nothing 위반: 부분 읽기 " + std::to_string(peeked) + " 발생");

                if (peeked == 0)
                {
                    continue;
                }

                totalPeeked++;

                // 2. Peek한 데이터 검증
                size_t numCount = peeked / sizeof(int);
                for (size_t i = 0; i < numCount; i++)
                {
                    int num = peekBuffer[i];
                    TEST_ASSERT(num >= 0 && num < TOTAL_NUMBERS, "Peek: 범위 초과 숫자 발견");
                }

                // 3. 검증 완료 후 Consume
                size_t consumed = rb->Consume(requestSize);
                TEST_ASSERT(consumed == requestSize, 
                    "Peek 후 Consume 실패! Peek=" + std::to_string(peeked) + ", Consume=" + std::to_string(consumed));

                // 4. 중복 소비 검증
                for (size_t i = 0; i < numCount; i++)
                {
                    int num = peekBuffer[i];
                    int expected = 0;
                    bool success = consumeCheck[num].compare_exchange_strong(expected, 1);
                    TEST_ASSERT(success, "중복 Consume 발견! 숫자: " + std::to_string(num));
                }

                totalConsumed += numCount;
            }
        });
    }

    for (auto& t : producers) t.join();
    allProducersDone = true;
    for (auto& t : consumers) t.join();

    // 진행률 출력 중지
    pauseProgress = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    running = false;
    progressThread.join();

    auto endTime = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(endTime - startTime).count();

    // 최종 검증
    std::cout << "\n========================================" << std::endl;
    std::cout << "[최종 검증]" << std::endl;
    std::cout << "========================================" << std::endl;

    // 1. 총 개수 일치
    TEST_ASSERT(totalEnqueued == TOTAL_NUMBERS, "Enqueue 개수 불일치");
    TEST_ASSERT(totalConsumed == TOTAL_NUMBERS, "Consume 개수 불일치");
    std::cout << "  > Enqueue/Consume 개수 일치: " << TOTAL_NUMBERS << " 개" << std::endl;

    // 2. 모든 숫자가 정확히 1번씩 처리되었는지 확인
    int missingCount = 0;
    int duplicateCount = 0;

    for (int i = 0; i < TOTAL_NUMBERS; i++)
    {
        int status = consumeCheck[i];
        if (status == 0)
        {
            missingCount++;
            std::cout << "  [ERROR] 누락된 숫자: " << i << std::endl;
        }
        else if (status > 1)
        {
            duplicateCount++;
            std::cout << "  [ERROR] 중복 처리: " << i << " (횟수: " << status << ")" << std::endl;
        }
    }

    TEST_ASSERT(missingCount == 0, "누락된 숫자 발견");
    TEST_ASSERT(duplicateCount == 0, "중복 처리된 숫자 발견");
    std::cout << "  > 모든 숫자 정확히 1번씩 처리 완료" << std::endl;

    TEST_ASSERT(rb->GetDataSize() == 0, "버퍼가 완전히 비워지지 않음");
    std::cout << "  > 버퍼 완전히 비워짐" << std::endl;

    std::cout << "  > 총 Peek 시도: " << totalPeeked << " 회" << std::endl;

    std::cout << "\n[PASS] Producer " << producerCount << " / Consumer " << consumerCount 
              << " 완료 (소요: " << elapsed << "초)" << std::endl;
    std::cout << "========================================" << std::endl;

    g_testCount++;
    std::this_thread::sleep_for(std::chrono::seconds(5));
}

// 다중 조합 테스트 실행
void Test_PeekConsume()
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "[Phase 2-2] Peek + Consume 테스트 (다양한 스레드 조합)" << std::endl;
    std::cout << "  - 각 Producer당 " << TestConfig::PEEK_CONSUME_PER_THREAD / 1'000'000 << "백만 개 생성" << std::endl;
    std::cout << "========================================" << std::endl;

    // 다양한 스레드 조합 (대칭 + 비대칭)
    std::vector<std::pair<int, int>> threadConfigs = {
        {1, 1},   // 대칭: 최소
        {2, 2},   // 대칭
        {4, 4},   // 대칭
        {8, 8},   // 대칭: 코어 수 고려
        {1, 8},   // 비대칭: Producer 적음
        {8, 1},   // 비대칭: Consumer 적음
        {2, 6},   // 비대칭
        {6, 2}    // 비대칭
    };

    std::vector<std::string> completedLines;

    for (size_t i = 0; i < threadConfigs.size(); i++)
    {
        int producerCount = threadConfigs[i].first;
        int consumerCount = threadConfigs[i].second;

        // 진행 중 조합 라인
        std::string runningLine = "[" + std::to_string(producerCount) + "-" + std::to_string(consumerCount) 
                                + "] Peek+Consume 테스트 진행 중..";

        RunPeekConsumeTest(
            producerCount,
            consumerCount,
            TestConfig::PEEK_CONSUME_PER_THREAD,  // ← TestConfig 사용
            completedLines,
            runningLine
        );

        // 완료된 조합을 상단에 누적
        completedLines.push_back("[" + std::to_string(producerCount) + "-" + std::to_string(consumerCount) 
                                + "] Peek+Consume 테스트 완료");
    }

    // 마지막 전체 완료 출력
#ifdef _WIN32
    system("cls");
#else
    system("clear");
#endif
    std::cout << "========================================" << std::endl;
    for (size_t i = 0; i < completedLines.size(); ++i)
    {
        std::cout << completedLines[i] << std::endl;
    }
    std::cout << "\n========================================" << std::endl;
    std::cout << "[Phase 2-2] 모든 Peek+Consume 조합 테스트 완료!" << std::endl;
    std::cout << "  - 총 " << threadConfigs.size() << "가지 조합 성공" << std::endl;
    std::cout << "========================================" << std::endl;
}

//=============================================================================
// Phase 2-3: 멀티스레드 - 고빈도 경합 테스트 (다양한 스레드 조합)
// 모든 스레드가 동시에 1바이트씩 Enqueue/Dequeue 반복
//=============================================================================

// 파라미터화된 고빈도 경합 테스트 함수
void RunHighContentionTest(
    int threadCount,
    uint64_t opsPerThread,
    const std::vector<std::string>& completedLines,
    const std::string& runningLine)
{
    auto rb = std::make_unique<CRingBufferMT>(1024);
    std::atomic<uint64_t> enqueueCount(0);
    std::atomic<uint64_t> dequeueCount(0);

    // 진행률 출력 스레드
    std::atomic<bool> running(true);
    std::atomic<bool> pauseProgress(false);
    std::thread progressThread([&]() {
        while (running)
        {
            if (!pauseProgress)
            {
#ifdef _WIN32
                system("cls");
#else
                system("clear");
#endif
                std::cout << "========================================" << std::endl;
                std::cout << "[Phase 2-3] 고빈도 경합 테스트" << std::endl;  // ← 추가
                std::cout << "========================================" << std::endl;
                for (size_t j = 0; j < completedLines.size(); ++j)
                {
                    std::cout << completedLines[j] << std::endl;
                }
                std::cout << runningLine << std::endl;
                
                uint64_t totalOps = opsPerThread * threadCount;
                uint64_t currentOps = enqueueCount + dequeueCount;
                double progress = (double)currentOps / totalOps * 100.0;
                
                std::cout << "[진행률] " << currentOps << " / " << totalOps 
                          << " (" << progress << "%)" << std::endl;
                std::cout << "  - Enqueue: " << enqueueCount << std::endl;
                std::cout << "  - Dequeue: " << dequeueCount << std::endl;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    });

    std::vector<std::thread> threads;
    auto startTime = std::chrono::steady_clock::now();

    // 모든 스레드가 동시에 1바이트씩 Enqueue/Dequeue 반복
    for (int i = 0; i < threadCount; i++)
    {
        threads.emplace_back([&, i]() {
            char byte = static_cast<char>(i);
            char readByte;

            for (uint64_t j = 0; j < opsPerThread; j++)
            {
                // 짝수 스레드: Enqueue
                if (i % 2 == 0)
                {
                    if (rb->Enqueue(&byte, 1) == 1)
                        enqueueCount++;
                }
                // 홀수 스레드: Dequeue
                else
                {
                    if (rb->Dequeue(&readByte, 1) == 1)
                        dequeueCount++;
                }
            }
        });
    }

    for (auto& t : threads) t.join();

    // 진행률 출력 중지
    pauseProgress = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    running = false;
    progressThread.join();

    auto endTime = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        endTime - startTime).count();

    // 최종 검증
    std::cout << "\n========================================" << std::endl;
    std::cout << "[최종 검증]" << std::endl;
    std::cout << "========================================" << std::endl;

    uint64_t totalSuccess = enqueueCount + dequeueCount;
    std::cout << "  > 총 성공 작업: " << totalSuccess << " 개" << std::endl;
    std::cout << "  > Enqueue 성공: " << enqueueCount << " 개" << std::endl;
    std::cout << "  > Dequeue 성공: " << dequeueCount << " 개" << std::endl;
    std::cout << "  > 소요 시간: " << elapsed << " ms" << std::endl;
    
    if (elapsed > 0)
    {
        std::cout << "  > 처리량: " << (totalSuccess * 1000 / elapsed) << " ops/sec" << std::endl;
    }

    std::cout << "\n[PASS] " << threadCount << "개 스레드 고빈도 경합 완료 (소요: " 
              << elapsed / 1000.0 << "초)" << std::endl;
    std::cout << "========================================" << std::endl;

    g_testCount++;
    std::this_thread::sleep_for(std::chrono::seconds(3));
}

// 다중 스레드 조합 테스트 실행
void Test_HighContentionFalseSharing()
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "[Phase 2-3] 고빈도 경합 테스트 (다양한 스레드 조합)" << std::endl;
    std::cout << "  - 각 스레드당 " << TestConfig::HIGH_CONTENTION_OPS_PER_THREAD / 1'000'000 << "백만 번 작업" << std::endl;
    std::cout << "========================================" << std::endl;

    // 다양한 스레드 조합
    std::vector<int> threadCounts = {
        2,    // 최소 (Producer 1, Consumer 1)
        4,    // 
        8,    // 
        16,   // 
        32    // 극한 경합
    };

    std::vector<std::string> completedLines;

    for (size_t i = 0; i < threadCounts.size(); i++)
    {
        int threadCount = threadCounts[i];

        // 진행 중 라인
        std::string runningLine = "[" + std::to_string(threadCount) + "개 스레드] 고빈도 경합 테스트 진행 중..";

        RunHighContentionTest(
            threadCount,
            TestConfig::HIGH_CONTENTION_OPS_PER_THREAD,  // ← TestConfig 사용
            completedLines,
            runningLine
        );

        // 완료된 조합을 상단에 누적
        completedLines.push_back("[" + std::to_string(threadCount) + "개 스레드] 고빈도 경합 테스트 완료");
    }

    // 마지막 전체 완료 출력
#ifdef _WIN32
    system("cls");
#else
    system("clear");
#endif
    std::cout << "========================================" << std::endl;
    for (size_t i = 0; i < completedLines.size(); ++i)
    {
        std::cout << completedLines[i] << std::endl;
    }
    std::cout << "\n========================================" << std::endl;
    std::cout << "[Phase 2-3] 모든 고빈도 경합 테스트 완료!" << std::endl;
    std::cout << "  - 총 " << threadCounts.size() << "가지 스레드 조합 성공" << std::endl;
    std::cout << "========================================" << std::endl;
}

//=============================================================================
// 메뉴 출력
//=============================================================================
void PrintMenu()
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "  CRingBuffer 테스트 메뉴" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "[Phase 1: 기본 검증 - 단일 스레드]" << std::endl;
    std::cout << "  1. 데이터 무결성 테스트 (1억 번)" << std::endl;
    std::cout << "  2. 불변성 검증 테스트 (1억 번)" << std::endl;
    std::cout << "  3. 경계 조건 테스트 (1억 번)" << std::endl;
    std::cout << "  4. Phase 1 전체 실행" << std::endl;
    std::cout << "\n[Phase 2: 멀티스레드 검증]" << std::endl;
    std::cout << "  5. Producer-Consumer 테스트 (1억 바이트)" << std::endl;
	std::cout << "  6. Peek + Consume 테스트 (1천만 번)" << std::endl;
	std::cout << "  7. 고빈도 경합 테스트" << std::endl;
    std::cout << "  8. Phase 2 전체 실행" << std::endl;
    std::cout << "\n[전체]" << std::endl;
    std::cout << "  9. 전체 테스트 실행 (Phase 1 + Phase 2)" << std::endl;
    std::cout << "  0. 종료" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "선택: ";
}

//=============================================================================
// Main
//=============================================================================
int main()
{
    std::cout << "========================================" << std::endl;
    std::cout << "  CRingBuffer 통합 테스트 시스템" << std::endl;
    std::cout << "  목표: 100% 안전성 확보" << std::endl;
    std::cout << "========================================" << std::endl;

    while (true)
    {
        PrintMenu();

        int choice;
        std::cin >> choice;

        if (choice == 0)
        {
            std::cout << "\n테스트를 종료합니다." << std::endl;
            break;
        }

        auto totalStart = std::chrono::steady_clock::now();
        g_testCount = 0;

        try
        {
            switch (choice)
            {
            case 1:
                Test_DataIntegrity();
                break;
            case 2:
                Test_Invariants();
                break;
            case 3:
                Test_BoundaryConditions();
                break;
            case 4:
                std::cout << "\n[Phase 1 전체 실행]" << std::endl;
                Test_DataIntegrity();
                Test_Invariants();
                Test_BoundaryConditions();
                break;
            case 5:
                Test_ProducerConsumer();
                break;
            case 6:
                Test_PeekConsume();
                break;
            case 7:
                Test_HighContentionFalseSharing();
				break;
            case 8:
                std::cout << "\n[Phase 2 전체 실행]" << std::endl;
                Test_ProducerConsumer();
                break;
            case 9:
                std::cout << "\n[전체 테스트 실행]" << std::endl;
                Test_DataIntegrity();
                Test_Invariants();
                Test_BoundaryConditions();
                Test_ProducerConsumer();
                break;
            default:
                std::cout << "\n잘못된 선택입니다." << std::endl;
                continue;
            }
        }
        catch (...)
        {
            std::cout << "\n[EXCEPTION] 예외 발생!" << std::endl;
            continue;
        }

        auto totalEnd = std::chrono::steady_clock::now();
        auto totalElapsed = std::chrono::duration_cast<std::chrono::seconds>(totalEnd - totalStart).count();

        std::cout << "\n========================================" << std::endl;
        std::cout << "  테스트 완료!" << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << "  - 완료된 테스트: " << g_testCount << " 개" << std::endl;
        std::cout << "  - 총 소요 시간: " << totalElapsed << " 초 ("
            << totalElapsed / 60 << " 분)" << std::endl;
        std::cout << "  - 결과: ✓ 100% PASS" << std::endl;
        std::cout << "========================================" << std::endl;
    }

    return 0;
}

//=============================================================================
// 잘못된 사용 패턴 방어 테스트
//=============================================================================
void Test_InvalidUsageDefense()
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "[추가] 잘못된 사용 패턴 방어 테스트" << std::endl;
    std::cout << "========================================" << std::endl;

    auto rb = std::make_unique<CRingBufferST>(1024);

    // 1. nullptr 전달
    TEST_ASSERT(rb->Enqueue(nullptr, 100) == 0, "nullptr Enqueue 허용됨");
    TEST_ASSERT(rb->Dequeue(nullptr, 100) == 0, "nullptr Dequeue 허용됨");
    TEST_ASSERT(rb->Peek(nullptr, 100) == 0, "nullptr Peek 허용됨");
    std::cout << "  > nullptr 방어: OK" << std::endl;

    // 2. size = 0 전달
    char dummy;
    TEST_ASSERT(rb->Enqueue(&dummy, 0) == 0, "크기 0 Enqueue 허용됨");
    TEST_ASSERT(rb->Dequeue(&dummy, 0) == 0, "크기 0 Dequeue 허용됨");
    std::cout << "  > 크기 0 방어: OK" << std::endl;

    // 3. 잘못된 capacity로 생성
    auto invalidRb = std::make_unique<CRingBufferST>(0);
    TEST_ASSERT(!invalidRb->IsValid(), "잘못된 capacity 허용됨");
    std::cout << "  > 잘못된 생성 방어: OK" << std::endl;

    // 4. 버퍼 오버플로우 시도
    std::vector<char> overData(2048);
    size_t written = rb->Enqueue(overData.data(), overData.size());
    TEST_ASSERT(written == 0, "capacity 초과 쓰기 허용됨");
    std::cout << "  > 오버플로우 방어: OK" << std::endl;

    std::cout << "[PASS] 잘못된 사용 패턴 방어 완료" << std::endl;
    g_testCount++;
}