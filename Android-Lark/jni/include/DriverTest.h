#include <iostream>
#include <string>
#include <limits>
#include <iomanip>  
#include <chrono>   
#include <random>  
#include <unistd.h> 
#include <stdlib.h>
#include "../include/DriverMemory.h"

class DriverTest
{
public:
    DriverTest() : m_pid(0) {}

    ~DriverTest()
    {
        // 析构处理
    }

    void Run()
    {
        bool running = true;
        while (running)
        {
            ShowMenu();
            int choice = Input<int>("请输入选项: ");

            switch (choice)
            {
            case 200:
                m_dr.ExitKernelThread();
                break;
            case 0:

                running = false;
                std::cout << "程序已退出。\n";
                break;
            case 1:
                HandleRead();
                break;
            case 2:
                HandleWrite();
                break;
            case 3:
                HandleGetModule();
                break;
            case 4:
                HandlePerformanceTest();
                break;
            case 5:
                UpdatePid();
                break;
            default:
                std::cout << "无效选项。\n";
                break;
            }

            if (running)
            {
                WaitForInput();
                ClearScreen();
            }
        }
    }

private:
    Driver m_dr;
    int m_pid;

    void HandleRead()
    {
        if (!CheckPid())
            return;

        std::cout << "请输入目标地址 (HEX): ";
        unsigned long long addr;
        if (!(std::cin >> std::hex >> addr))
        {
            FixInputStream();
            return;
        }

        int val = m_dr.Read<int>(addr);

        std::cout << "----------------------------------\n";
        std::cout << "地址: 0x" << std::hex << addr << "\n";
        std::cout << "数值(Dec): " << std::dec << val << "\n";
        std::cout << "数值(Hex): 0x" << std::hex << val << "\n";
        std::cout << "----------------------------------\n";
    }

    void HandleWrite()
    {
        if (!CheckPid())
            return;

        std::cout << "请输入目标地址 (HEX): ";
        unsigned long long addr;
        if (!(std::cin >> std::hex >> addr))
        {
            FixInputStream();
            return;
        }

        std::cout << "请输入写入数值 (Dec): ";
        long val;
        if (!(std::cin >> std::dec >> val))
        {
            FixInputStream();
            return;
        }

        m_dr.Write(addr, val);

        int check = m_dr.Read<int>(addr);
        std::cout << "写入完成。复查值: " << std::dec << check << "\n";
    }

    void HandleGetModule()
    {
        if (!CheckPid())
            return;

        std::cout << "请输入模块名称: ";
        std::string name;
        std::cin >> name;

        int seg = Input<int>("模块区段 (1:代码, 2:只读, 3:可读写): ");

        unsigned long long baseAddr = 0;
        m_dr.GetModuleBase(name.c_str(), seg, &baseAddr);

        std::cout << "模块 [" << name << "] 基址: 0x" << std::hex << baseAddr << "\n";
    }

    // 性能测
    void HandlePerformanceTest()
    {
        int oldPid = m_pid;
        int selfPid = getpid();

        std::cout << "========== 性能测试 (No Warm-up) ==========\n";
        std::cout << "测试对象: 自身进程 (PID=" << selfPid << ")\n";


        int *pMem = new int(0);
        unsigned long long targetAddr = reinterpret_cast<unsigned long long>(pMem);

        // 生成随机验证码
        std::random_device rd;
        std::mt19937 gen(rd());
        int testVal = std::uniform_int_distribution<int>(100000, 999999)(gen);
        *pMem = testVal;

        // 切换驱动目标
        m_dr.SetQJPid(selfPid);

        // 直接开始压测 (无预热检查)
        const int loopCount = 1000000;
        volatile int failCount = 0;

        std::cout << "正在执行 " << std::dec << loopCount << " 次读取...\n";

        auto start = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < loopCount; ++i)
        {
            if (m_dr.Read<int>(targetAddr) != testVal)
            {
                failCount++;
            }
        }

        auto end = std::chrono::high_resolution_clock::now();

        // 统计
        double totalMs = std::chrono::duration<double, std::milli>(end - start).count();
        double avgUs = (totalMs * 1000) / loopCount;
        double qps = (loopCount / totalMs) * 1000;

        std::cout << "========== 测试结果 ==========\n";
        std::cout << "总耗时  : " << std::fixed << std::setprecision(2) << totalMs << " ms\n";
        std::cout << "平均延迟: " << std::setprecision(4) << avgUs << " us\n";
        std::cout << "吞吐量  : " << std::setprecision(0) << qps << " ops/sec\n";

        if (failCount == 0)
        {
            std::cout << "完整性  : [通过] 数据全部正确\n";
        }
        else
        {
            std::cout << "完整性  : [失败] 错误次数: " << failCount << "\n";
            if (failCount == loopCount)
            {
                std::cout << "警告    : 所有读取均失败，请检查驱动是否支持读取自身。\n";
            }
        }

        auto start_io = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < loopCount; ++i)
        {
            m_dr.NULLIO();
        }

        auto end_io = std::chrono::high_resolution_clock::now();

        // 统计 NULLIO
        double totalMs_io = std::chrono::duration<double, std::milli>(end_io - start_io).count();
        double avgUs_io = (totalMs_io * 1000) / loopCount;
        double qps_io = (loopCount / totalMs_io) * 1000;

        std::cout << "完成\n";
        std::cout << ">>> NULLIO 结果 (通信链路极限):\n";
        std::cout << "总耗时  : " << std::fixed << std::setprecision(2) << totalMs_io << " ms\n";
        std::cout << "单次延迟: " << std::setprecision(4) << avgUs_io << " us\n";
        std::cout << "极限吞吐: " << std::setprecision(0) << qps_io << " ops/sec\n";

        // 清理
        delete pMem;
        if (oldPid != 0)
            m_dr.SetQJPid(oldPid);
        else
            m_pid = 0;
    }

    void UpdatePid()
    {
        m_pid = Input<int>("请输入目标进程 PID: ");
        m_dr.SetQJPid(m_pid);
        std::cout << "PID 已更新为: " << m_pid << "\n";
    }

    void ShowMenu()
    {
        std::cout << "===================================\n";
        std::cout << "Linux Driver Memory Tester\n";
        std::cout << "当前 PID: " << (m_pid == 0 ? "未设置" : std::to_string(m_pid)) << "\n";
        std::cout << "-----------------------------------\n";
        std::cout << "0. 退出\n1. 读内存\n2. 写内存\n3. 模块基址\n4. 性能测试(100万次)\n5. 换PID\n";
        std::cout << "===================================\n";
    }

    bool CheckPid()
    {
        if (m_pid == 0)
        {
            std::cout << "请先设置目标 PID。\n";
            UpdatePid();
        }
        m_dr.SetQJPid(m_pid);
        return true;
    }

    template <typename T>
    T Input(const std::string &prompt)
    {
        T val;
        while (true)
        {
            std::cout << prompt;
            if (std::cin >> val)
                break;
            FixInputStream();
        }
        return val;
    }

    void FixInputStream()
    {
        std::cin.clear();
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        std::cout << "输入无效。\n";
    }

    void WaitForInput()
    {
        std::cout << "\n按回车继续...";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        std::cin.get();
    }

    void ClearScreen()
    {
        system("clear");
    }
};