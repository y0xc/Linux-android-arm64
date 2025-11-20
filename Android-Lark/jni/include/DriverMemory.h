#ifndef SYSCALL_READ_H
#define SYSCALL_READ_H

#include <cstdio>
#include <sys/mman.h>
#include <cerrno>
#include <cstring>
#include <atomic> // std::atomic 所需的头文件
#include <thread>
#include <sys/prctl.h> //用于修改程序名

class Driver
{
public:
    Driver()
    {
        initCommunication();
    }

    ~Driver()
    {
        ExitCommunication();
    }

    void ExitKernelThread()
    {
        m_ioPacket->Op = kexit;

        _io_commit_and_wait();
    }
    void NULLIO()
    {
        m_ioPacket->Op = op_o;
        _io_commit_and_wait();
    }

    int getPID(char *PackageName)
    {
        int pid;
        FILE *fp;
        char cmd[0x100] = "pidof ";
        strcat(cmd, PackageName);
        fp = popen(cmd, "r");
        fscanf(fp, "%d", &pid);
        pclose(fp);

        return pid;
    }
    void SetQJPid(pid_t pid)
    {
        QJPID = pid;
    }

    // 内核接口
    template <typename T>
    T Read(unsigned long long address)
    {
        T value = {};
        KReadProcessMemory(QJPID, address, &value, sizeof(T));
        return value;
    }
    bool Read(unsigned long long address, void *buffer, size_t size) { return KReadProcessMemory(QJPID, address, buffer, size); }

    template <typename T>
    bool Write(unsigned long long address, const T &value)
    {
        return KWriteProcessMemory(QJPID, address, const_cast<T *>(&value), sizeof(T));
    }
    bool Write(unsigned long long address, void *buffer, size_t size) { return KWriteProcessMemory(QJPID, address, buffer, size); }

    int GetModuleBase(const char MaduleName[46], short SegmentIndex, unsigned long long *ModuleAddr)
    {
        m_ioPacket->Op = op_m;
        m_ioPacket->TargetProcessId = QJPID;
        snprintf(m_ioPacket->ModuleName, 46, "%s", MaduleName);
        m_ioPacket->SegmentIndex = SegmentIndex;

        _io_commit_and_wait();

        *ModuleAddr = m_ioPacket->ModuleBaseAddress;
        return m_ioPacket->status == 0;
    }

    void v_touch_down(int x, int y)
    {
        m_ioPacket->Op = op_down;
        m_ioPacket->x = x;
        m_ioPacket->y = y;

        _io_commit_and_wait();
    }
    void v_touch_move(int x, int y)
    {
        m_ioPacket->Op = op_move;
        m_ioPacket->x = x;
        m_ioPacket->y = y;

        _io_commit_and_wait();
    }
    void v_touch_up()
    {
        m_ioPacket->Op = op_up;

        _io_commit_and_wait();
    }

private:
    // 定义请求操作的枚举类型
    typedef enum _req_op
    {
        op_o = 0, // 空调用
        op_r = 1,
        op_w = 2,
        op_m = 3,

        op_down = 4,
        op_move = 5,
        op_up = 6,

        exit = 100,
        kexit = 200
    } req_op;

    // 将在队列中使用的请求实例结构体
    typedef struct _req_Obj
    {
        std::atomic<int> kernel; // 由用户模式设置 1 = 内核有待处理的请求, 0 = 请求已完成
        std::atomic<int> user;   // 由内核模式设置 1 = 用户模式有待处理的请求, 0 = 请求已完成

        req_op Op;  // 请求操作类型
        int status; // 操作状态

        // 内存读取
        int TargetProcessId;
        unsigned long long TargetAddress;
        int TransferSize;
        char UserBufferAddress[1024];

        // 模块基地址获取
        char ModuleName[46];
        short SegmentIndex; // 模块区段
        unsigned long long ModuleBaseAddress;
        unsigned long long ModuleSize;

        // 触摸坐标
        int x, y;

    } Requests;

    Requests *m_ioPacket; // 共享内存指针
    pid_t QJPID;          // 全局pid

    inline void _io_commit_and_wait()
    {
        // 通知内核有新的请求
        m_ioPacket->kernel.store(1, std::memory_order_release);

        // 等待内核处理完成的信号
        // 使用简单的 load() 循环检查，直到 user 变为 1。
        // memory_order_acquire 确保在读取到 1 之后，内核对共享内存（如返回数据）的所有写入都对我们可见。
        while (m_ioPacket->user.load(std::memory_order_acquire) != 1)
        {
        };

        // 成功接收到信号，重置 user 标志位为 0，告知内核我们已收到响应
        m_ioPacket->user.store(0, std::memory_order_relaxed);
    }

    void initCommunication()
    {
        // 修改程序名为Lark
        prctl(PR_SET_NAME, "Lark", 0, 0, 0);
        // 使用类成员变量 m_ioPacket
        m_ioPacket = (Requests *)mmap((void *)0x2025827000, sizeof(Requests), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (m_ioPacket == MAP_FAILED)
        {
            printf("[-] 分配共享内存失败，错误码: %d (%s)\n", errno, strerror(errno));
            return;
        }
        memset(m_ioPacket, 0, sizeof(Requests));

        printf("[+] 分配虚拟地址成功，地址: %p\n", m_ioPacket);
        printf("当前进程 PID: %d\n", getpid());
        printf("等待驱动握手...\n");

        // 连接比较特殊要单独等
        while (m_ioPacket->user.load(std::memory_order_acquire) != 1)
        {
        };
        m_ioPacket->user.store(0, std::memory_order_relaxed);

        printf("驱动已经连接\n");
    }

    void ExitCommunication()
    {

        printf("正在通知驱动断开连接...\n");

        m_ioPacket->Op = exit;

        _io_commit_and_wait();
    }

    int KReadProcessMemory(pid_t pid, unsigned long long addr, void *buffer, size_t size)
    {
        m_ioPacket->Op = op_r;
        m_ioPacket->TargetProcessId = pid;
        m_ioPacket->TargetAddress = addr;
        m_ioPacket->TransferSize = size;

        _io_commit_and_wait();

        switch (size)
        {
        case 4:
            // 最常见的 int, float, 指针(32位)
            *reinterpret_cast<uint32_t *>(buffer) = *reinterpret_cast<uint32_t *>(m_ioPacket->UserBufferAddress);
            break;
        case 8:
            // 最常见的 long long, double, 指针(64位)
            *reinterpret_cast<uint64_t *>(buffer) = *reinterpret_cast<uint64_t *>(m_ioPacket->UserBufferAddress);
            break;
        case 1:
            *reinterpret_cast<uint8_t *>(buffer) = *reinterpret_cast<uint8_t *>(m_ioPacket->UserBufferAddress);
            break;
        case 2:
            *reinterpret_cast<uint16_t *>(buffer) = *reinterpret_cast<uint16_t *>(m_ioPacket->UserBufferAddress);
            break;
        default:
            // 处理 3, 5, 6, 7 或其他奇葩长度
            memcpy(buffer, m_ioPacket->UserBufferAddress, size);
            break;
        }

        return m_ioPacket->status == 0; // 内核返回0表示没有错误
    }
    int KWriteProcessMemory(pid_t pid, unsigned long long addr, void *buffer, size_t size)
    {
        m_ioPacket->Op = op_w;
        m_ioPacket->TargetProcessId = pid;
        m_ioPacket->TargetAddress = addr;
        m_ioPacket->TransferSize = size;

        switch (size)
        {
        case 4:
            *reinterpret_cast<uint32_t *>(m_ioPacket->UserBufferAddress) = *reinterpret_cast<uint32_t *>(buffer);
            break;
        case 8:
            *reinterpret_cast<uint64_t *>(m_ioPacket->UserBufferAddress) = *reinterpret_cast<uint64_t *>(buffer);
            break;
        case 1:
            *reinterpret_cast<uint8_t *>(m_ioPacket->UserBufferAddress) = *reinterpret_cast<uint8_t *>(buffer);
            break;
        case 2:
            *reinterpret_cast<uint16_t *>(m_ioPacket->UserBufferAddress) = *reinterpret_cast<uint16_t *>(buffer);
            break;
        default:
            memcpy(m_ioPacket->UserBufferAddress, buffer, size);
            break;
        }

        _io_commit_and_wait();

        return m_ioPacket->status == 0; // 内核会返回0表示没有错误
    }
};

#endif
