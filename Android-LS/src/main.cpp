#include <stdio.h>
#include <iostream>
#include <vector>
#include <list>
#include <thread>
#include <atomic>
#include <memory>
#include <string>
#include <cstdint>
#include <cstdlib>
#include <set>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <map>
#include <sstream>
#include <fstream>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <ranges>
#include <format>
#include <concepts>
#include <variant>
#include <optional>
#include <charconv>
#include <unordered_set>
#include <stack>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <numeric>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <print>
#include <utility>
#include <numeric>
#include <cinttypes>
#include <atomic>
#include <algorithm>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "DriverMemory.h"
#include "Android_draw/draw.h"
#include "imgui.h"
#include "ImGuiFloatingKeyboard.h"
#include "Disassembler.h"

// ============================================================================
// 配置模块 (Config)
// ============================================================================
namespace Config
{
    inline std::atomic<bool> g_Running{true};
    inline std::atomic<int> g_ItemsPerPage{100};

    struct Constants
    {
        static constexpr size_t MEM_VIEW_RANGE = 50;
        static constexpr size_t SCAN_BUFFER = 4096;
        static constexpr size_t BATCH_SIZE = 16384;
        static constexpr size_t MAX_READ_GAP = 64;
        static constexpr double FLOAT_EPSILON = 1e-4;
        static constexpr uintptr_t ADDR_MIN = 0x10000;
        static constexpr uintptr_t ADDR_MAX = 0x7FFFFFFFFFFF;
    };

    inline unsigned GetThreadCount() noexcept
    {
        if (auto n = std::thread::hardware_concurrency(); n > 0)
            return n;
        return 4;
    }

}

namespace Utils
{
    class ThreadPool
    {
        std::vector<std::jthread> workers_;
        std::queue<std::function<void()>> tasks_;
        std::mutex mtx_;
        std::condition_variable_any cv_;
        std::condition_variable done_cv_;
        size_t active_{0};

    public:
        explicit ThreadPool(size_t n = Config::GetThreadCount())
        {
            if (n == 0)
                n = 4;
            for (size_t i = 0; i < n; ++i)
            {
                workers_.emplace_back([this](std::stop_token st)
                                      {
                while (!st.stop_requested()) {
                    std::function<void()> task;
                    {
                        std::unique_lock lk(mtx_);
                        cv_.wait(lk, st, [&]{ return !tasks_.empty(); });
                    if(st.stop_requested()) return;
                        if (tasks_.empty()) continue;
                        task = std::move(tasks_.front());
                        tasks_.pop();
                        ++active_;
                    }
                    task();
                    {
                        std::lock_guard lk(mtx_);
                        --active_;
                        if (tasks_.empty() && active_ == 0)
                            done_cv_.notify_all();
                    }
                } });
            }
        }

        template <class F, class... Args>
        auto push(F &&f, Args &&...args) -> std::future<std::invoke_result_t<F, Args...>>
        {
            using R = std::invoke_result_t<F, Args...>;
            auto task = std::make_shared<std::packaged_task<R()>>(
                std::bind(std::forward<F>(f), std::forward<Args>(args)...));
            auto fut = task->get_future();
            {
                std::lock_guard lk(mtx_);
                tasks_.emplace([task]
                               { (*task)(); });
            }
            cv_.notify_one();
            return fut;
        }

        void wait_all()
        {
            std::unique_lock lk(mtx_);
            done_cv_.wait(lk, [&]
                          { return tasks_.empty() && active_ == 0; });
        }

        // 强行终止所有线程，不等待任务完成
        void force_stop()
        {
            {
                std::lock_guard lk(mtx_);
                while (!tasks_.empty()) // 清空待执行任务
                    tasks_.pop();
            }
            for (auto &w : workers_) // 请求所有线程停止
                w.request_stop();
            cv_.notify_all();
            for (auto &w : workers_) // detach 所有线程，不等待
            {
                if (w.joinable())
                    w.detach();
            }
            workers_.clear();
        }
    };

    // 定义全局唯一的线程池实例
    inline ThreadPool GlobalPool{Config::GetThreadCount()};
}

// ============================================================================
// 类型定义 (Types)
// ============================================================================
namespace Types
{
    enum class DataType : uint8_t
    {
        I8 = 0,
        I16,
        I32,
        I64,
        Float,
        Double,
        Count
    };

    enum class FuzzyMode : uint8_t
    {
        Unknown = 0,
        Equal,
        Greater,
        Less,
        Increased,
        Decreased,
        Changed,
        Unchanged,
        Range,
        Count
    };

    enum class ViewFormat : uint8_t
    {
        Hex = 0,
        Hex64,
        I8,
        I16,
        I32,
        I64,
        Float,
        Double,
        Disasm,
        Count
    };

    struct MemNode
    {
        uintptr_t addr;
        uintptr_t value;
        auto operator<=>(const MemNode &) const = default;
    };

    namespace Labels
    {
        constexpr std::array TYPE = {"Int8", "Int16", "Int32", "Int64", "Float", "Double"};
        constexpr std::array FUZZY = {"未知", "等于", "大于", "小于", "增加", "减少", "改变", "不变", "范围"};
        constexpr std::array FORMAT = {"HexDump", "Hex64", "I8", "I16", "I32", "I64", "Float", "Double", "Disasm"};
    }

    constexpr std::array<size_t, 6> DATA_SIZES = {1, 2, 4, 8, 4, 8};
    constexpr std::array<size_t, 9> VIEW_SIZES = {1, 8, 1, 2, 4, 8, 4, 8, 4};

    constexpr size_t GetDataSize(DataType type) noexcept
    {
        auto idx = std::to_underlying(type);
        return idx < DATA_SIZES.size() ? DATA_SIZES[idx] : 1;
    }

    constexpr size_t GetViewSize(ViewFormat fmt) noexcept
    {
        auto idx = std::to_underlying(fmt);
        return idx < VIEW_SIZES.size() ? VIEW_SIZES[idx] : 1;
    }
}

// ============================================================================
// 内存工具 (Memory Utils)
// ============================================================================
namespace MemUtils
{
    using namespace Types;
    using namespace Config;

    // 去除MTE指针标签0xb40000
    constexpr uintptr_t Normalize(uintptr_t addr) noexcept
    {
        return addr & 0xFFFFFFFFFFFFULL;
    }

    // 验证地址合法性，指针和地址才需要验证，值不需要
    constexpr bool IsValidAddr(uintptr_t addr) noexcept
    {
        return addr > Constants::ADDR_MIN && addr < Constants::ADDR_MAX;
    }

    // 辅助分发
    template <typename F>
    decltype(auto) DispatchType(DataType type, F &&fn)
    {
        switch (type)
        {
        case DataType::I8:
            return fn.template operator()<int8_t>();
        case DataType::I16:
            return fn.template operator()<int16_t>();
        case DataType::I32:
            return fn.template operator()<int32_t>();
        case DataType::I64:
            return fn.template operator()<int64_t>();
        case DataType::Float:
            return fn.template operator()<float>();
        case DataType::Double:
            return fn.template operator()<double>();
        default:
            return fn.template operator()<int32_t>();
        }
    }

    // 读取并格式化为字符串
    inline std::string ReadAsString(uintptr_t addr, DataType type)
    {
        if (!addr)
            return "??";
        return DispatchType(type, [&]<typename T>() -> std::string
                            {
        T val = dr.Read<T>(addr);
        if constexpr (std::is_floating_point_v<T>)
            return std::format("{:.11f}", val);
        else if constexpr (sizeof(T) <= 4)
            return std::to_string(static_cast<int>(val));
        else
            return std::to_string(static_cast<long long>(val)); });
    }
    // 字符串解析写入
    inline bool WriteFromString(uintptr_t addr, DataType type, std::string_view str)
    {
        if (!addr || str.empty())
            return false;
        try
        {
            std::string s(str);
            return DispatchType(type, [&]<typename T>() -> bool
                                {
            if constexpr (std::is_same_v<T, float>)
                return dr.Write<T>(addr, std::stof(s));
            else if constexpr (std::is_same_v<T, double>)
                return dr.Write<T>(addr, std::stod(s));
            else if constexpr (sizeof(T) <= 4)
                return dr.Write<T>(addr, static_cast<T>(std::stoi(s)));
            else
                return dr.Write<T>(addr, static_cast<T>(std::stoll(s))); });
        }
        catch (...)
        {
            return false;
        }
    }

    template <typename T>
    bool Compare(T value, T target, FuzzyMode mode, double lastValue, double rangeMax = 0.0)
    {

        if constexpr (std::is_integral_v<T>)
        { // 整数使用精确比较
            T last = static_cast<T>(lastValue);
            switch (mode)
            {
            case FuzzyMode::Equal:
                return value == target;
            case FuzzyMode::Greater:
                return value > target;
            case FuzzyMode::Less:
                return value < target;
            case FuzzyMode::Increased:
                return value > last;
            case FuzzyMode::Decreased:
                return value < last;
            case FuzzyMode::Changed:
                return value != last;
            case FuzzyMode::Unchanged:
                return value == last;
            case FuzzyMode::Range:
            {
                T lo = target, hi = static_cast<T>(rangeMax);
                if (lo > hi)
                    std::swap(lo, hi);
                return value >= lo && value <= hi;
            }
            default:
                return false;
            }
        }
        else
        {
            constexpr double eps = Constants::FLOAT_EPSILON;
            double v = static_cast<double>(value);
            double t = static_cast<double>(target);
            switch (mode)
            {
            case FuzzyMode::Equal:
                return std::abs(v - t) < eps;
            case FuzzyMode::Greater:
                return value > target;
            case FuzzyMode::Less:
                return value < target;
            case FuzzyMode::Increased:
                return value > static_cast<T>(lastValue);
            case FuzzyMode::Decreased:
                return value < static_cast<T>(lastValue);
            case FuzzyMode::Changed:
                return std::abs(v - lastValue) > eps;
            case FuzzyMode::Unchanged:
                return std::abs(v - lastValue) < eps;
            case FuzzyMode::Range:
            {
                double lo = t, hi = rangeMax;
                if (lo > hi)
                    std::swap(lo, hi); // 自动纠正反向输入
                return v >= lo - eps && v <= hi + eps;
            }
            default:
                return false;
            }
        }
    }

    struct OffsetParseResult
    {
        uintptr_t offset;
        bool negative;
    };

    // 解析输入的HEX字符串
    inline std::optional<OffsetParseResult> ParseHexOffset(std::string_view str)
    {
        if (str.empty())
            return std::nullopt;

        size_t pos = 0;
        while (pos < str.size() && str[pos] == ' ')
            ++pos;
        if (pos >= str.size())
            return std::nullopt;

        bool negative = false;
        if (str[pos] == '-')
        {
            negative = true;
            ++pos;
        }
        else if (str[pos] == '+')
        {
            ++pos;
        }

        if (pos >= str.size())
            return std::nullopt;

        // 跳过0x前缀
        if (pos + 1 < str.size() && str[pos] == '0' && (str[pos + 1] == 'x' || str[pos + 1] == 'X'))
        {
            pos += 2;
        }

        uintptr_t offset = 0;
        std::string sub(str.substr(pos));
        if (std::sscanf(sub.c_str(), "%lx", &offset) != 1)
        {
            return std::nullopt;
        }

        return OffsetParseResult{offset, negative};
    }
}

// ============================================================================
// 内存扫描器 (Memory Scanner)
// ============================================================================
class MemScanner
{
public:
    using Results = std::vector<uintptr_t>;
    using Values = std::vector<double>;

private:
    Results results_;
    Values lastValues_;
    mutable std::shared_mutex mutex_;
    std::atomic<float> progress_{0.0f};
    std::atomic<bool> scanning_{false};
    double rangeMax_ = 0.0;

public:
    bool isScanning() const noexcept { return scanning_; }
    float progress() const noexcept { return progress_; }

    size_t count() const
    {
        std::shared_lock lock(mutex_);
        return results_.size();
    }

    Results getPage(size_t start, size_t cnt) const
    {
        std::shared_lock lock(mutex_);
        if (start >= results_.size())
            return {};
        auto endIdx = std::min(start + cnt, results_.size());
        return Results(results_.begin() + start, results_.begin() + endIdx);
    }

    void clear()
    {
        std::unique_lock lock(mutex_);
        results_.clear();
        lastValues_.clear();
    }

    void remove(uintptr_t addr)
    {
        std::unique_lock lock(mutex_);
        auto it = std::ranges::lower_bound(results_, addr);
        if (it != results_.end() && *it == addr)
        {
            auto idx = std::distance(results_.begin(), it);
            results_.erase(it);
            if (static_cast<size_t>(idx) < lastValues_.size())
            {
                lastValues_.erase(lastValues_.begin() + idx);
            }
        }
    }

    void add(uintptr_t addr)
    {
        std::unique_lock lock(mutex_);
        auto it = std::ranges::lower_bound(results_, addr);
        if (it == results_.end() || *it != addr)
        {
            auto idx = std::distance(results_.begin(), it);
            results_.insert(it, addr);
            lastValues_.insert(lastValues_.begin() + idx, 0.0);
        }
    }

    void applyOffset(int64_t offset)
    {
        std::unique_lock lock(mutex_);
        // 构建索引数组用于排序同步
        std::vector<size_t> idx(results_.size());
        std::iota(idx.begin(), idx.end(), 0);

        // 先偏移
        for (auto &a : results_)
        {
            a = offset > 0 ? a + static_cast<uintptr_t>(offset)
                           : a - static_cast<uintptr_t>(-offset);
        }

        // 排序并同步 lastValues
        std::ranges::sort(idx, {}, [&](size_t i)
                          { return results_[i]; });

        Results newR(results_.size());
        Values newV(lastValues_.size());
        for (size_t i = 0; i < idx.size(); ++i)
        {
            newR[i] = results_[idx[i]];
            if (idx[i] < lastValues_.size())
                newV[i] = lastValues_[idx[i]];
        }
        results_ = std::move(newR);
        lastValues_ = std::move(newV);
    }

    template <typename T>
    void scan(pid_t pid, T target, Types::FuzzyMode mode, bool isFirst, double rangeMax = 0.0)
    {
        if (scanning_.exchange(true))
            return;
        struct Guard
        {
            std::atomic<bool> &s;
            std::atomic<float> &p;
            ~Guard()
            {
                s = false;
                p = 1.0f;
            }
        } guard{scanning_, progress_};

        progress_ = 0.0f;
        rangeMax_ = rangeMax;

        if (isFirst)
            scanFirst<T>(pid, target, mode);
        else
            scanNext<T>(target, mode);
    }

private:
    template <typename T>
    void scanFirst(pid_t pid, T target, Types::FuzzyMode mode)
    {
        auto regions = dr.GetScanRegions();
        if (regions.empty())
            return;

        unsigned tc = std::min(static_cast<size_t>(Config::GetThreadCount()), regions.size());
        std::vector<Results> tR(tc);
        std::vector<Values> tV(tc);
        std::atomic<size_t> done{0};
        size_t chunk = (regions.size() + tc - 1) / tc;
        double rmx = rangeMax_;

        std::vector<std::future<void>> futs;
        futs.reserve(tc);

        for (unsigned t = 0; t < tc; ++t)
        {
            futs.push_back(Utils::GlobalPool.push([&, t, rmx]
                                                  {
            size_t beg = t * chunk;
            size_t end = std::min(beg + chunk, regions.size());
            tR[t].reserve(100000);
            tV[t].reserve(100000);
            std::vector<uint8_t> buf(Config::Constants::SCAN_BUFFER);

            for (size_t i = beg; i < end && Config::g_Running; ++i) {
                auto [rStart, rEnd] = regions[i];
                if (rEnd - rStart < sizeof(T)) continue;

                for (uintptr_t addr = rStart; addr < rEnd; addr += Config::Constants::SCAN_BUFFER) {
                    size_t sz = std::min(static_cast<size_t>(rEnd - addr),
                                         Config::Constants::SCAN_BUFFER);
                    if (dr.Read(addr, buf.data(), sz) != 0) continue;

                    for (size_t off = 0; off + sizeof(T) <= sz; off += sizeof(T)) {
                        T value;
                        std::memcpy(&value, buf.data() + off, sizeof(T));
                        bool match = (mode == Types::FuzzyMode::Unknown) ||
                                     MemUtils::Compare(value, target, mode, 0, rmx);
                        if (match) {
                            tR[t].push_back(addr + off);
                            tV[t].push_back(static_cast<double>(value));
                        }
                    }
                }
                if ((done.fetch_add(1) & 0x7F) == 0)
                    progress_ = static_cast<float>(done) / regions.size();
            } }));
        }

        for (auto &f : futs)
            f.get();
        mergeResults(tR, tV, tc);
    }

    template <typename T>
    void scanNext(T target, Types::FuzzyMode mode)
    {
        Results oldResults;
        Values oldValues;
        {
            std::unique_lock lock(mutex_);
            if (results_.empty())
                return;
            oldResults = std::move(results_);
            oldValues = std::move(lastValues_);
        }

        unsigned threadCount = Config::GetThreadCount();
        size_t total = oldResults.size();
        size_t chunk = (total + threadCount - 1) / threadCount;

        std::vector<Results> threadResults(threadCount);
        std::vector<Values> threadValues(threadCount);
        std::vector<std::future<void>> futures;
        std::atomic<size_t> done{0};
        double rangeMax = rangeMax_;

        for (unsigned t = 0; t < threadCount; ++t)
        {
            futures.push_back(Utils::GlobalPool.push([&, t, rangeMax]
                                                     {
            size_t start = t * chunk;
            size_t end = std::min(start + chunk, total);
            if (start >= end) return;
            threadResults[t].reserve((end - start) / 3);
            threadValues[t].reserve((end - start) / 3);
            std::vector<uint8_t> buffer;

            for (size_t i = start; i < end && Config::g_Running;) {
                uintptr_t batchStart = oldResults[i];
                size_t j = i + 1;
                while (j < end
                    && oldResults[j] - oldResults[j-1] <= Config::Constants::MAX_READ_GAP
                    && oldResults[j] - batchStart + sizeof(T) <= Config::Constants::BATCH_SIZE)
                {
                    ++j;
                }
                size_t bytes = (oldResults[j-1] - batchStart) + sizeof(T);
                buffer.resize(bytes);
                if (dr.Read(batchStart, buffer.data(), bytes) == 0) {
                    for (size_t k = i; k < j; ++k) {
                        T value = *reinterpret_cast<T*>(buffer.data() + (oldResults[k] - batchStart));
                        if (MemUtils::Compare(value, target, mode,
                                oldValues[k], rangeMax))
                        {
                            threadResults[t].push_back(oldResults[k]);
                            threadValues[t].push_back(static_cast<double>(value));
                        }
                    }
                }
                done += (j - i);
                i = j;
                if (t == 0)
                    progress_ = static_cast<float>(done) / total;
            } }));
        }

        for (auto &f : futures)
            f.get();
        mergeResults(threadResults, threadValues, threadCount);
    }

    void mergeResults(std::vector<Results> &threadResults, std::vector<Values> &threadValues, unsigned threadCount)
    {
        size_t total = 0;
        std::vector<size_t> offsets(threadCount);
        for (size_t i = 0; i < threadCount; ++i)
        {
            offsets[i] = total;
            total += threadResults[i].size();
        }

        std::unique_lock lock(mutex_);
        results_.resize(total);
        lastValues_.resize(total);

        for (unsigned t = 0; t < threadCount; ++t)
        {
            if (threadResults[t].empty())
                continue;
            std::ranges::copy(threadResults[t],
                              results_.begin() + offsets[t]);
            std::ranges::copy(threadValues[t],
                              lastValues_.begin() + offsets[t]);
        }
    }
};

// ============================================================================
// 指针管理器 (Pointer Scanner)
// ============================================================================
class PointerManager
{
public:
    // 指针数据：记录一个内存地址及其存储的值
    struct PtrData
    {
        uintptr_t address, value;
        PtrData() : address(0), value(0) {}
        PtrData(uintptr_t a, uintptr_t v) : address(a), value(v) {}
    };

    // 指针目录项：带有子节点索引范围的指针数据
    struct PtrDir
    {
        uintptr_t address, value;
        uint32_t start, end; // 指向下一层节点的索引范围 [start, end)
        PtrDir() : address(0), value(0), start(0), end(0) {}
        PtrDir(uintptr_t a, uintptr_t v, uint32_t s = 0, uint32_t e = 0)
            : address(a), value(v), start(s), end(e) {}
    };

    // 指针范围：一组归属于某个基址的指针集合
    struct PtrRange
    {
        int level;                   // 所在层级
        int moduleIdx = -1;          // 驱动memory_info.modules 的索引
        int segIdx = -1;             // 驱动module_info.segs 的索引
        bool isManual;               // 是否手动基址模式
        bool isArray;                // 是否数组模式
        uintptr_t manualBase;        // 手动基址
        uintptr_t arrayBase;         // 数组基址
        size_t arrayIndex;           // 数组索引
        std::vector<PtrDir> results; // 该范围内的所有指针
        PtrRange() : level(0), moduleIdx(-1), segIdx(-1), isManual(false),
                     isArray(false), manualBase(0), arrayBase(0), arrayIndex(0) {}
    };

    // 二进制文件头
    struct BinHeader
    {
        char sign[32];
        int module_count;
        int version;
        int size;
        int level;
        uint8_t scanBaseMode;
        uint64_t scanManualBase;
        uint64_t scanArrayBase;
        uint64_t scanArrayCount;
        uint64_t scanTarget;
    };

    // 二进制符号项
    struct BinSym
    {
        uint64_t start;      // 起始地址（模块基址/对象地址）
        char name[128];      // 名称
        int segment;         // 段索引
        int pointer_count;   // 指针数量
        int level;           // 层级
        bool isBss;          // 是否BSS段
        uint8_t sourceMode;  // 来源模式：0=模块, 1=手动, 2=数组
        uint64_t manualBase; // 手动基址
        uint64_t arrayBase;  // 数组基址
        uint64_t arrayIndex; // 数组索引
    };

    // 二进制层级项
    struct BinLevel
    {
        unsigned int count; // 该层节点数量
        int level;          // 层级编号
    };

    // 基址模式枚举
    enum class BaseMode : int
    {
        Module = 0, // 模块基址
        Manual,     // 手动指定基址
        Array       // 指针数组
    };

private:
    // ==================== 成员变量 ====================
    std::mutex block_mtx_;                                 // 缓冲块互斥锁
    std::condition_variable block_cv_;                     // 缓冲块条件变量
    std::vector<PtrData> pointers_;                        // 收集到的所有指针
    std::vector<std::pair<uintptr_t, uintptr_t>> regions_; // 内存区域列表
    std::atomic<bool> scanning_{false};                    // 是否正在扫描
    std::atomic<float> scanProgress_{0.0f};                // 扫描进度
    size_t chainCount_ = 0;                                // 链总数

    static std::string NextBinName()
    {
        char path[256];
        snprintf(path, sizeof(path), "Pointer.bin");
        if (access(path, F_OK) != 0)
            return path;

        for (int i = 1; i < 9999; i++)
        {
            snprintf(path, sizeof(path), "Pointer_%d.bin", i);
            if (access(path, F_OK) != 0)
                return path;
        }
        return "Pointer.bin";
    }
    // 带缓冲块的操作封装
    template <typename F>
    void with_buffer_block(char **bufs, int &idx, uintptr_t start, size_t len, F &&call)
    {
        char *buf;
        {
            std::unique_lock<std::mutex> lk(block_mtx_);
            block_cv_.wait(lk, [&idx]
                           { return idx >= 0; });
            buf = bufs[idx--];
        }

        call(buf, start, len);

        {
            std::lock_guard<std::mutex> lk(block_mtx_);
            bufs[++idx] = buf;
        }
        block_cv_.notify_one();
    }
    // 收集一个内存块中的指针
    void collect_pointers_block(char *buf, uintptr_t start, size_t len, FILE *&out)
    {
        out = tmpfile();
        if (!out)
            return;

        // 读取内存块
        if (dr.Read(start, buf, len))
        {
            fclose(out);
            out = nullptr;
            return;
        }

        size_t ptr_count = len / sizeof(uintptr_t);
        uintptr_t *vals = reinterpret_cast<uintptr_t *>(buf);

        // 归一化所有值
        for (size_t i = 0; i < ptr_count; i++)
            vals[i] = MemUtils::Normalize(vals[i]);

        // 计算有效地址范围
        uintptr_t min_addr = regions_.front().first;
        uintptr_t max_addr = regions_.back().second;
        uintptr_t sub = max_addr - min_addr;

        PtrData d;
        for (size_t i = 0; i < ptr_count; i++)
        {
            uintptr_t v = vals[i];

            // 快速范围检查
            if ((v - min_addr) > sub)
                continue;

            // 二分查找所属区域
            int lo = 0, hi = static_cast<int>(regions_.size()) - 1;
            while (lo <= hi)
            {
                int mid = (lo + hi) >> 1;
                if (regions_[mid].second <= v)
                    lo = mid + 1;
                else
                    hi = mid - 1;
            }

            if (static_cast<size_t>(lo) >= regions_.size() || v < regions_[lo].first)
                continue;

            // 记录有效指针
            d.address = start + i * sizeof(uintptr_t);
            d.value = v;
            fwrite(&d, sizeof(d), 1, out);
        }
        fflush(out);
    }

    // 通用二分查找
    template <typename C, typename F, typename V>
    static void bin_search(C &c, F &&cmp, V target, size_t sz, int &lo, int &hi)
    {
        lo = 0;
        hi = static_cast<int>(sz) - 1;
        while (lo <= hi)
        {
            int mid = (lo + hi) >> 1;
            if (cmp(c[mid], target))
                lo = mid + 1;
            else
                hi = mid - 1;
        }
    }

    // 在指针集合中搜索指向目标的指针
    void search_in_pointers(std::vector<PtrDir> &input, std::vector<PtrData *> &out, size_t offset, bool use_limit, size_t limit)
    {
        if (input.empty() || pointers_.empty())
            return;

        uintptr_t min_addr = regions_.front().first;
        uintptr_t max_addr = regions_.back().second;
        uintptr_t sub = max_addr - min_addr;
        size_t isz = input.size();
        std::vector<PtrData *> result;

        for (auto &pd : pointers_)
        {
            uintptr_t v = MemUtils::Normalize(pd.value);
            if ((v - min_addr) > sub)
                continue;

            int lo, hi;
            bin_search(input, [](auto &n, auto t)
                       { return n.address < t; }, v, isz, lo, hi);

            if (static_cast<size_t>(lo) >= isz)
                continue;

            uintptr_t diff = MemUtils::Normalize(input[lo].address) - v;
            if (diff > offset)
                continue;

            result.push_back(&pd);
        }

        size_t lim = use_limit ? std::min(limit, result.size()) : result.size();
        out.reserve(lim);
        for (size_t i = 0; i < lim; i++)
            out.push_back(result[i]);
    }

    // 按模块过滤指针到范围
    void filter_to_ranges_module(std::vector<std::vector<PtrDir>> &dirs, std::vector<PtrRange> &ranges, std::vector<PtrData *> &curr, int level, const std::string &filterModule)
    {
        std::unordered_set<PtrData *> matched;
        const auto &info = dr.GetMemoryInfoRef();

        for (int mi = 0; mi < info.module_count; ++mi)
        {
            const auto &mod = info.modules[mi];
            std::string_view fullPath(mod.name);
            auto slash = fullPath.rfind('/');
            std::string_view fileName = (slash != std::string_view::npos)
                                            ? fullPath.substr(slash + 1)
                                            : fullPath;

            if (!filterModule.empty() &&
                fileName.find(filterModule) == std::string_view::npos)
                continue;

            for (int si = 0; si < mod.seg_count; ++si)
            {
                const auto &seg = mod.segs[si];

                PtrRange pr;
                pr.level = level;
                pr.moduleIdx = mi;
                pr.segIdx = si;
                pr.isManual = false;
                pr.isArray = false;
                for (auto *p : curr)
                {
                    if (p->address >= seg.start && p->address < seg.end)
                    {
                        // 现在 insert 自动去重，返回 .second 表示是否新插入
                        if (matched.insert(p).second)
                        {
                            pr.results.emplace_back(
                                MemUtils::Normalize(p->address),
                                MemUtils::Normalize(p->value), 0u, 1u);
                        }
                    }
                }

                // 空的也会 push
                if (!pr.results.empty())
                    ranges.push_back(std::move(pr));
            }
        }
        push_unmatched(dirs, matched, curr, level);
    }

    // 组合过滤：同时检查模块、手动基址、数组
    void filter_to_ranges_combined(std::vector<std::vector<PtrDir>> &dirs, std::vector<PtrRange> &ranges, std::vector<PtrData *> &curr, int level, BaseMode scanMode, const std::string &filterModule, uintptr_t manualBase, size_t manualMaxOffset, uintptr_t arrayBase, const std::vector<std::pair<size_t, uintptr_t>> &arrayEntries, size_t maxOffset)
    {

        std::unordered_set<PtrData *> matched;
        const auto &info = dr.GetMemoryInfoRef();

        // 模块基址匹配部分
        for (int mi = 0; mi < info.module_count; ++mi)
        {
            const auto &mod = info.modules[mi];
            std::string_view fullPath(mod.name);
            auto slash = fullPath.rfind('/');
            std::string_view fileName = (slash != std::string_view::npos)
                                            ? fullPath.substr(slash + 1)
                                            : fullPath;

            if (!filterModule.empty() &&
                fileName.find(filterModule) == std::string_view::npos)
                continue;

            for (int si = 0; si < mod.seg_count; ++si)
            {
                const auto &seg = mod.segs[si];

                PtrRange pr;
                pr.level = level;
                pr.moduleIdx = mi;
                pr.segIdx = si;
                pr.isManual = false;
                pr.isArray = false;

                for (auto *p : curr)
                {
                    if (p->address >= seg.start && p->address < seg.end)
                    {

                        if (matched.find(p) == matched.end())
                        {
                            matched.insert(p);
                            pr.results.emplace_back(
                                MemUtils::Normalize(p->address),
                                MemUtils::Normalize(p->value),
                                0u, 1u);
                        }
                    }
                }
                if (!pr.results.empty())
                    ranges.push_back(std::move(pr));
            }
        }

        // 手动模式：检查手动基址
        if (scanMode == BaseMode::Manual && manualBase)
        {
            PtrRange pr;
            pr.level = level;
            pr.moduleIdx = -1;
            pr.segIdx = -1;
            pr.isManual = true;
            pr.isArray = false;
            pr.manualBase = manualBase;

            for (auto *p : curr)
            {
                uintptr_t addr = MemUtils::Normalize(p->address);

                if (addr >= manualBase && (addr - manualBase) <= manualMaxOffset)
                {
                    if (matched.find(p) == matched.end())
                    {
                        matched.insert(p);
                        pr.results.emplace_back(
                            addr,
                            MemUtils::Normalize(p->value),
                            0u, 1u);
                    }
                }
            }

            if (!pr.results.empty())
                ranges.push_back(std::move(pr));
        }

        // 数组模式：检查所有数组元素
        if (scanMode == BaseMode::Array && !arrayEntries.empty())
        {
            for (const auto &[idx, objAddr] : arrayEntries)
            {
                PtrRange pr;
                pr.level = level;
                pr.moduleIdx = -1;
                pr.segIdx = -1;
                pr.isManual = false;
                pr.isArray = true;
                pr.arrayBase = arrayBase;
                pr.arrayIndex = idx;

                for (auto *p : curr)
                {
                    uintptr_t addr = MemUtils::Normalize(p->address);

                    if (addr >= objAddr && (addr - objAddr) <= maxOffset)
                    {
                        if (matched.find(p) == matched.end())
                        {
                            matched.insert(p);
                            pr.results.emplace_back(
                                addr,
                                MemUtils::Normalize(p->value),
                                0u, 1u);
                        }
                    }
                }

                if (!pr.results.empty())
                    ranges.push_back(std::move(pr));
            }
        }

        // 未匹配的放入 dirs
        push_unmatched(dirs, matched, curr, level);
    }

    // 将未匹配的指针放入dirs供下一层使用
    void push_unmatched(std::vector<std::vector<PtrDir>> &dirs, std::unordered_set<PtrData *> &matched, std::vector<PtrData *> &curr, int level)
    {
        for (auto *p : curr)
        {
            if (matched.find(p) == matched.end())
                dirs[level].emplace_back(MemUtils::Normalize(p->address),
                                         MemUtils::Normalize(p->value), 0u, 1u);
        }
    }

    // 关联索引：建立节点与下层节点的引用关系
    void assoc_index(std::vector<PtrDir> &prev, PtrDir *start, size_t count, size_t offset)
    {
        size_t sz = prev.size();
        for (size_t i = 0; i < count; i++)
        {
            auto *d = &start[i];
            uintptr_t nv = MemUtils::Normalize(d->value);
            int lo, hi;

            // 找到 value 对应的起始索引
            bin_search(prev, [](auto &x, auto t)
                       { return x.address < t; }, nv, sz, lo, hi);
            d->start = lo;

            // 找到 value + offset 对应的结束索引
            bin_search(prev, [](auto &x, auto t)
                       { return x.address <= t; }, nv + offset, sz, lo, hi);
            d->end = lo;
        }
    }

    // 并行创建关联索引
    std::vector<std::future<void>> create_assoc_index(std::vector<PtrDir> &prev, std::vector<PtrDir> &curr, size_t offset)
    {
        std::vector<std::future<void>> futures;
        if (curr.empty())
            return futures;
        size_t total = curr.size();
        size_t avg = 10000;
        size_t pos = 0;
        while (pos < total)
        {
            size_t chunk = std::min(total - pos, avg);
            PtrDir *s = &curr[pos];
            futures.push_back(Utils::GlobalPool.push(
                [this, &prev, s, chunk, offset]
                {
                    assoc_index(prev, s, chunk, offset);
                }));
            pos += chunk;
        }
        return futures;
    }

    // 目录树结构
    struct DirTree
    {
        std::vector<std::vector<size_t>> counts;     // 各层链数量前缀和
        std::vector<std::vector<PtrDir *>> contents; // 各层实际使用的节点
        bool valid;
        DirTree() : valid(false) {}
    };

    // 合并目录节点
    void merge_dirs(std::vector<PtrDir *> &sorted_ptrs, PtrDir *base_dir, FILE *f)
    {
        size_t dist = 0;
        uint32_t right = 0;

        for (auto *p : sorted_ptrs)
        {
            auto &s = p->start;
            auto &e = p->end;

            if (right <= s)
            {
                dist += s - right;
                for (uint32_t j = s; j < e; j++)
                {
                    PtrDir *d = &base_dir[j];
                    fwrite(&d, sizeof(d), 1, f);
                }
                right = e;
            }
            else if (right < e)
            {
                for (uint32_t j = right; j < e; j++)
                {
                    PtrDir *d = &base_dir[j];
                    fwrite(&d, sizeof(d), 1, f);
                }
                right = e;
            }
            s -= static_cast<uint32_t>(dist);
            e -= static_cast<uint32_t>(dist);
        }
    }

    // 构建目录树
    DirTree build_dir_tree(std::vector<std::vector<PtrDir>> &dirs, std::vector<PtrRange> &ranges)
    {
        DirTree tree;
        if (ranges.empty())
            return tree;

        // 找到最大层级
        int max_level = 0;
        for (auto &r : ranges)
            max_level = std::max(max_level, r.level);

        // 按层级分组
        std::vector<std::vector<PtrRange *>> level_ranges(dirs.size());
        for (auto &r : ranges)
            level_ranges[r.level].push_back(&r);

        tree.counts.resize(max_level + 1);
        tree.contents.resize(max_level + 1);

        // 自底向上构建
        for (int i = max_level; i > 0; i--)
        {
            FILE *f = tmpfile();
            if (!f)
                return tree;

            std::vector<PtrDir *> stn;
            for (auto *r : level_ranges[i])
                for (auto &v : r->results)
                    stn.push_back(&v);
            for (auto *p : tree.contents[i])
                stn.push_back(p);

            std::sort(stn.begin(), stn.end(),
                      [](auto a, auto b)
                      { return a->start < b->start; });

            merge_dirs(stn, dirs[i - 1].data(), f);
            fflush(f);

            struct stat st;
            fstat(fileno(f), &st);
            size_t cnt = st.st_size / sizeof(PtrDir *);

            if (cnt == 0)
            {
                fclose(f);
                return tree;
            }

            PtrDir **mapped = reinterpret_cast<PtrDir **>(
                mmap(nullptr, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fileno(f), 0));
            if (mapped == MAP_FAILED)
            {
                fclose(f);
                return tree;
            }

            tree.contents[i - 1].assign(mapped, mapped + cnt);
            munmap(mapped, st.st_size);
            fclose(f);
        }

        // 计算链数量前缀和
        tree.counts[0] = {0, 1};
        for (int i = 1; i <= max_level; i++)
        {
            auto &cc = tree.counts[i];
            auto &pc = tree.counts[i - 1];
            auto &content = tree.contents[i - 1];
            size_t c = 0;
            cc.reserve(content.size() + 1);
            cc.push_back(c);
            for (size_t j = 0; j < content.size(); j++)
            {
                c += pc[content[j]->end] - pc[content[j]->start];
                cc.push_back(c);
            }
        }

        tree.valid = true;
        return tree;
    }

    // 写入bin文件（扫描结果）
    void write_bin_file(std::vector<std::vector<PtrDir *>> &contents, std::vector<PtrRange> &ranges, FILE *f, BaseMode scanMode, uintptr_t target, uintptr_t manualBase, uintptr_t arrayBase, size_t arrayCount)
    {
        const auto &memInfo = dr.GetMemoryInfoRef();
        // 写入文件头
        BinHeader hdr{};
        strcpy(hdr.sign, ".bin pointer chain");
        hdr.size = sizeof(uintptr_t);
        hdr.version = 102;
        hdr.module_count = static_cast<int>(ranges.size());
        hdr.level = static_cast<int>(contents.size()) - 1;
        hdr.scanBaseMode = static_cast<uint8_t>(scanMode);
        hdr.scanManualBase = manualBase;
        hdr.scanArrayBase = arrayBase;
        hdr.scanArrayCount = arrayCount;
        hdr.scanTarget = target;
        fwrite(&hdr, sizeof(hdr), 1, f);
        // 写入符号和根节点
        for (auto &r : ranges)
        {
            BinSym sym{};
            if (r.isManual)
            {
                sym.sourceMode = 1;
                sym.manualBase = r.manualBase;
                sym.start = r.manualBase;
                strncpy(sym.name, "manual", sizeof(sym.name) - 1);
                sym.segment = 0;
                sym.isBss = false;
            }
            else if (r.isArray)
            {
                sym.sourceMode = 2;
                sym.arrayBase = r.arrayBase;
                sym.arrayIndex = r.arrayIndex;
                uintptr_t elemAddr = r.arrayBase + r.arrayIndex * sizeof(uintptr_t);
                uintptr_t objAddr = 0;
                dr.Read(elemAddr, &objAddr, sizeof(objAddr));
                objAddr = MemUtils::Normalize(objAddr);
                sym.start = objAddr;
                char arrName[128];
                snprintf(arrName, sizeof(arrName), "array[%zu]", r.arrayIndex);
                strncpy(sym.name, arrName, sizeof(sym.name) - 1);
                sym.segment = 0;
                sym.isBss = false;
            }
            else
            {
                const auto &mod = memInfo.modules[r.moduleIdx];
                const auto &seg = mod.segs[r.segIdx];
                sym.start = seg.start;
                sym.segment = seg.index;
                sym.isBss = (seg.index == -1);

                // 提取文件名
                std::string_view fullPath(mod.name);
                auto slash = fullPath.rfind('/');
                std::string_view fileName = (slash != std::string_view::npos)
                                                ? fullPath.substr(slash + 1)
                                                : fullPath;
                strncpy(sym.name, fileName.data(),
                        std::min(fileName.size(), sizeof(sym.name) - 1));
                sym.sourceMode = 0;
            }
            sym.level = r.level;
            sym.pointer_count = static_cast<int>(r.results.size());
            fwrite(&sym, sizeof(sym), 1, f);
            fwrite(r.results.data(), sizeof(PtrDir), r.results.size(), f);
        }
        // 写入各层节点
        for (size_t i = 0; i + 1 < contents.size(); i++)
        {
            BinLevel ll{};
            ll.level = static_cast<int>(i);
            ll.count = static_cast<unsigned int>(contents[i].size());
            fwrite(&ll, sizeof(ll), 1, f);
            for (auto *p : contents[i])
                fwrite(p, sizeof(PtrDir), 1, f);
        }
        fflush(f);
    }

public:
    PointerManager() = default;

    ~PointerManager() = default;

    // ==================== 状态查询接口 ====================

    bool isScanning() const noexcept { return scanning_; }
    float scanProgress() const noexcept { return scanProgress_; }
    size_t count() const noexcept { return chainCount_; }

    // ==================== 收集指针 ====================
    size_t CollectPointers(int buf_count = 10, int buf_size = 1 << 20)
    {
        pointers_.clear();
        if (regions_.empty())
            return 0;
        int idx = buf_count - 1;
        std::vector<char *> bufs(buf_count);
        for (int i = 0; i < buf_count; i++)
            bufs[i] = new char[buf_size];
        std::vector<FILE *> tmp_files;
        std::mutex tmp_mtx;
        std::vector<std::future<void>> futures;
        for (auto &[rstart, rend] : regions_)
        {
            uintptr_t pos = rstart;
            while (pos < rend)
            {
                size_t chunk = std::min(static_cast<size_t>(rend - pos),
                                        static_cast<size_t>(buf_size));
                futures.push_back(Utils::GlobalPool.push(
                    [this, &bufs, &idx, pos, chunk, &tmp_files, &tmp_mtx]
                    {
                        FILE *out = nullptr;
                        with_buffer_block(bufs.data(), idx, pos, chunk,
                                          [this, &out](char *buf, uintptr_t s, size_t l)
                                          {
                                              collect_pointers_block(buf, s, l, out);
                                          });
                        if (out)
                        {
                            std::lock_guard<std::mutex> lk(tmp_mtx);
                            tmp_files.push_back(out);
                        }
                    }));
                pos += chunk;
            }
        }
        for (auto &f : futures)
            f.get();

        // 合并临时文件
        FILE *merged = tmpfile();
        auto *mbuf = new char[1 << 20];
        for (auto *tf : tmp_files)
        {
            rewind(tf);
            size_t sz;
            while ((sz = fread(mbuf, 1, 1 << 20, tf)) > 0)
                fwrite(mbuf, sz, 1, merged);
            fclose(tf);
        }
        delete[] mbuf;
        fflush(merged);

        // 读取合并结果
        struct stat st;
        fstat(fileno(merged), &st);
        size_t total = st.st_size / sizeof(PtrData);
        if (total > 0)
        {
            pointers_.resize(total);
            rewind(merged);
            fread(pointers_.data(), sizeof(PtrData), total, merged);
        }
        fclose(merged);

        for (int i = 0; i < buf_count; i++)
            delete[] bufs[i];

        return pointers_.size();
    }

    // ==================== 扫描指针链 ====================
    void scan(pid_t pid, uintptr_t target, int depth, int maxOffset, bool useManual, uintptr_t manualBase, int manualMaxOffset, bool useArray, uintptr_t arrayBase, size_t arrayCount, const std::string &filterModule)
    {
        //  防止重复运行
        if (scanning_.exchange(true))
            return;

        struct ScanGuard
        {
            std::atomic<bool> &scanning;
            std::atomic<float> &progress;
            ~ScanGuard()
            {
                scanning = false;
                progress = 1.0f;
            }
        } guard{scanning_, scanProgress_};
        // 初始化状态
        scanProgress_ = 0.0f;
        chainCount_ = 0;

        std::println("=== 开始指针扫描 ===");
        std::println("目标: {:x}, 深度: {}, 偏移: {}", target, depth, maxOffset);

        //  获取内存并生成快照
        regions_ = dr.GetScanRegions();
        std::sort(regions_.begin(), regions_.end());

        size_t ptrCount = CollectPointers();
        if (pointers_.empty())
        {
            std::println(stderr, "扫描失败: 内存快照为空 (CollectPointers returned 0)");
            scanning_ = false;
            scanProgress_ = 1.0f;
            return;
        }
        std::println("内存快照数量: {}", ptrCount);

        // 准备容器
        BaseMode scanMode;
        if (useManual)
            scanMode = BaseMode::Manual;
        else if (useArray)
            scanMode = BaseMode::Array;
        else
            scanMode = BaseMode::Module;

        std::vector<uintptr_t> addrs;
        addrs.push_back(MemUtils::Normalize(target));

        FILE *outfile = tmpfile();
        if (!outfile)
        {
            std::println(stderr, "无法创建临时文件");
            scanning_ = false;
            scanProgress_ = 1.0f;
            return;
        }

        std::vector<PtrRange> ranges;
        std::vector<std::vector<PtrDir>> dirs(depth + 1);
        size_t fidx = 0, totalChains = 0;

        // 数组模式预处理
        std::vector<std::pair<size_t, uintptr_t>> arrayEntries;
        if (scanMode == BaseMode::Array && arrayBase && arrayCount > 0)
        {
            for (size_t i = 0; i < arrayCount; i++)
            {
                uintptr_t elemAddr = arrayBase + i * sizeof(uintptr_t);
                uintptr_t ptr = 0;
                if (dr.Read(elemAddr, &ptr, sizeof(ptr)) == 0)
                {
                    ptr = MemUtils::Normalize(ptr);
                    if (MemUtils::IsValidAddr(ptr))
                        arrayEntries.emplace_back(i, ptr);
                }
            }
        }

        // 显式处理 Level 0 (目标地址)
        {
            // 把目标地址加入到 dirs[0]
            dirs[0].reserve(addrs.size());
            for (auto a : addrs)
            {
                // PtrDir(address, value, start, end) -> value在此处不重要，设为0
                dirs[0].emplace_back(MemUtils::Normalize(a), 0, 0, 1);
            }

            // 必须排序！search_in_pointers 依赖二分查找
            std::sort(dirs[0].begin(), dirs[0].end(), [](const PtrDir &a, const PtrDir &b)
                      { return a.address < b.address; });

            std::println("Level 0 初始化完成，目标地址数量: {}", dirs[0].size());
        }

        // 收集所有future
        std::vector<std::future<void>> allFutures;

        // 循环扫描 Level 1 到 Level N
        for (int level = 1; level <= depth; level++)
        {
            std::vector<PtrData *> curr;

            // 在 pointers_ 中搜索指向 dirs[level-1] 的指针
            // search_in_pointers 内部使用二分查找，所以 dirs[level-1] 必须有序
            search_in_pointers(dirs[level - 1], curr, static_cast<size_t>(maxOffset), false, 0);

            if (curr.empty())
            {
                std::println("扫描在 Level {} 结束: 未找到指向上级的指针", level);
                break; // 链断了，停止扫描
            }

            std::println("Level {} 搜索结果: 找到 {} 个指针", level, curr.size());

            // 按地址排序，方便后续处理
            std::sort(curr.begin(), curr.end(), [](auto a, auto b)
                      { return a->address < b->address; });

            // 过滤并保存结果到 ranges 和 dirs[level]
            filter_to_ranges_combined(
                dirs,
                ranges,
                curr,
                level,
                scanMode,
                filterModule,
                manualBase,
                static_cast<size_t>(manualMaxOffset),
                arrayBase,
                arrayEntries,
                static_cast<size_t>(maxOffset));

            // 创建关联索引 (加速文件写入)
            auto futs = create_assoc_index(dirs[level - 1], dirs[level], static_cast<size_t>(maxOffset));
            for (auto &f : futs)
                allFutures.push_back(std::move(f));

            // 更新进度条
            scanProgress_ = static_cast<float>(level + 1) / (depth + 2);
        }

        // 后处理：生成文件// 对 ranges 里的结果也建立索引
        for (; fidx < ranges.size(); fidx++)
        {
            auto &r = ranges[fidx];
            if (r.level > 0)
            {
                auto futs = create_assoc_index(dirs[r.level - 1], r.results, static_cast<size_t>(maxOffset));
                for (auto &f : futs)
                    allFutures.push_back(std::move(f));
            }
        }

        if (!ranges.empty())
        {
            auto tree = build_dir_tree(dirs, ranges);
            if (tree.valid)
            {
                for (auto &r : ranges)
                {
                    // 安全检查，防止越界
                    if (r.level < tree.counts.size())
                    {
                        auto &cc = tree.counts[r.level];
                        for (auto &v : r.results)
                        {
                            if (v.end < cc.size() && v.start < cc.size())
                            {
                                totalChains += cc[v.end] - cc[v.start];
                            }
                        }
                    }
                }
                write_bin_file(tree.contents, ranges, outfile, scanMode, target, manualBase, arrayBase, arrayCount);
                std::println("文件写入完成，总链数: {}", totalChains);
            }
        }
        else
        {
            std::println("结果为空: ranges vector is empty");
        }

        // 保存到磁盘 (Pointer.bin)
        std::string autoName = NextBinName();
        FILE *saveFile = fopen(autoName.c_str(), "w+b");
        if (saveFile)
        {
            rewind(outfile);
            char buf[1 << 16];
            size_t sz;
            while ((sz = fread(buf, 1, sizeof(buf), outfile)) > 0)
                fwrite(buf, sz, 1, saveFile);
            fflush(saveFile);
            fclose(saveFile);
            std::println("结果已保存至: {}", autoName);
        }
        else
        {
            std::println(stderr, "无法保存文件: {}", autoName);
        }

        fclose(outfile);
        chainCount_ = totalChains;
    }

    void MergeBins()
    {
        std::println("=== [MergeBins] 开始合并 ===");

        // 链条指纹：仅偏移序列用于对比
        struct ChainFP
        {
            std::string modName;
            int segIdx; // 区段索引
            uint8_t sourceMode;
            int64_t baseOffset;           // 根相对基址的偏移
            std::vector<int64_t> offsets; // 路径偏移序列

            bool operator<(const ChainFP &o) const
            {
                if (modName != o.modName)
                    return modName < o.modName;
                if (segIdx != o.segIdx)
                    return segIdx < o.segIdx;
                if (sourceMode != o.sourceMode)
                    return sourceMode < o.sourceMode;
                if (baseOffset != o.baseOffset)
                    return baseOffset < o.baseOffset;
                return offsets < o.offsets;
            }
        };

        // 链条完整数据：写回用
        struct ChainData
        {
            BinSym sym;               // 完整符号信息（含 segment, isBss, name 等）
            PtrDir root;              // 根节点（含完整 address, value）
            std::vector<PtrDir> path; // 根的子节点到叶子的路径（不含根自身）
        };

        // ===== 1. 收集所有 bin 文件 =====
        std::vector<std::string> files;
        if (access("Pointer.bin", F_OK) == 0)
            files.push_back("Pointer.bin");
        for (int i = 1; i < 9999; ++i)
        {
            char buf[64];
            snprintf(buf, 64, "Pointer_%d.bin", i);
            if (access(buf, F_OK) == 0)
                files.push_back(buf);
            else if (i > 50)
                break;
        }

        if (files.size() < 2)
        {
            std::println("文件不足({})，跳过合并。", files.size());
            return;
        }

        // ===== 2. 通用解析函数 =====
        auto ParseFile = [&](const std::string &path,
                             std::map<ChainFP, ChainData> *fullOut,
                             std::set<ChainFP> *fpOut) -> bool
        {
            FILE *f = fopen(path.c_str(), "rb");
            if (!f)
                return false;

            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            rewind(f);
            std::vector<char> raw(sz);
            if ((long)fread(raw.data(), 1, sz, f) != sz)
            {
                fclose(f);
                return false;
            }
            fclose(f);

            char *cur = raw.data();
            char *eof = raw.data() + sz;

            if (cur + (long)sizeof(BinHeader) > eof)
                return false;
            BinHeader *hdr = (BinHeader *)cur;
            cur += sizeof(BinHeader);

            // 解析符号块
            struct Block
            {
                BinSym sym;
                std::vector<PtrDir> roots;
            };
            std::vector<Block> blocks;

            for (int i = 0; i < hdr->module_count; ++i)
            {
                if (cur + (long)sizeof(BinSym) > eof)
                    return false;
                BinSym *s = (BinSym *)cur;
                cur += sizeof(BinSym);

                long need = s->pointer_count * (long)sizeof(PtrDir);
                if (cur + need > eof)
                    return false;

                std::vector<PtrDir> roots(s->pointer_count);
                memcpy(roots.data(), cur, need);
                cur += need;
                blocks.push_back({*s, std::move(roots)});
            }

            // 解析层级节点
            int lvlCount = hdr->level + 1;
            std::vector<std::vector<PtrDir>> levels(lvlCount > 0 ? lvlCount : 1);
            while (cur + (long)sizeof(BinLevel) <= eof)
            {
                BinLevel *bl = (BinLevel *)cur;
                cur += sizeof(BinLevel);

                if (bl->level < 0 || bl->level >= (int)levels.size())
                    break;

                long need = bl->count * (long)sizeof(PtrDir);
                if (cur + need > eof)
                    break;

                levels[bl->level].resize(bl->count);
                memcpy(levels[bl->level].data(), cur, need);
                cur += need;
            }

            // DFS 提取链条
            std::vector<PtrDir> nodeStack;

            std::function<void(int, const PtrDir &, ChainFP &, const BinSym &, const PtrDir &)> dfs;
            dfs = [&](int lvl, const PtrDir &node, ChainFP &fp,
                      const BinSym &sym, const PtrDir &root)
            {
                // 叶子判断：无子节点
                bool leaf = (lvl < 0) ||
                            (lvl >= (int)levels.size()) ||
                            (node.start >= node.end);

                if (leaf)
                {
                    if (fpOut)
                        fpOut->insert(fp);
                    if (fullOut)
                    {
                        ChainData cd;
                        cd.sym = sym;
                        cd.root = root;
                        cd.path = nodeStack;
                        (*fullOut)[fp] = cd;
                    }
                    return;
                }

                const auto &layer = levels[lvl];
                for (uint32_t i = node.start; i < node.end && i < (uint32_t)layer.size(); ++i)
                {
                    const PtrDir &child = layer[i];
                    int64_t off = (int64_t)child.address - (int64_t)node.value;

                    fp.offsets.push_back(off);
                    if (fullOut)
                        nodeStack.push_back(child);

                    dfs(lvl - 1, child, fp, sym, root);

                    fp.offsets.pop_back();
                    if (fullOut)
                        nodeStack.pop_back();
                }
            };

            // 遍历所有符号块的所有根节点
            for (const auto &blk : blocks)
            {
                // 确定基址：与 scan/write_bin_file 中一致
                // sym.start = seg.start (模块模式)
                //           = manualBase (手动模式，在write中设置)
                //           = objAddr (数组模式，在write中设置)
                uint64_t baseAddr = blk.sym.start;
                if (blk.sym.sourceMode == 1)
                    baseAddr = blk.sym.manualBase;
                // sourceMode==2: sym.start 已经是 objAddr

                for (const auto &root : blk.roots)
                {
                    ChainFP fp;
                    fp.modName = blk.sym.name;
                    fp.segIdx = blk.sym.segment; // ← 保留区段索引
                    fp.sourceMode = blk.sym.sourceMode;
                    fp.baseOffset = (int64_t)root.address - (int64_t)baseAddr;

                    nodeStack.clear();
                    dfs(blk.sym.level - 1, root, fp, blk.sym, root);
                }
            }
            return true;
        };

        // ===== 3. 加载基准文件（完整数据） =====
        std::map<ChainFP, ChainData> survivors;
        std::println("加载基准: {}", files[0]);
        if (!ParseFile(files[0], &survivors, nullptr))
        {
            std::println(stderr, "加载失败: {}", files[0]);
            return;
        }
        std::println("  初始链条: {}", survivors.size());

        // ===== 4. 逐文件对比取交集 =====
        for (size_t i = 1; i < files.size(); ++i)
        {
            if (survivors.empty())
                break;

            std::println("对比: {}", files[i]);
            std::set<ChainFP> fpSet;
            if (!ParseFile(files[i], nullptr, &fpSet))
            {
                std::println(stderr, "  加载失败，跳过");
                continue;
            }

            for (auto it = survivors.begin(); it != survivors.end();)
            {
                if (fpSet.count(it->first) == 0)
                    it = survivors.erase(it);
                else
                    ++it;
            }
            std::println("  剩余: {}", survivors.size());
        }

        if (survivors.empty())
        {
            std::println("无共同链条，不生成文件。");
            return;
        }

        // ===== 5. 重建 bin 文件 =====
        std::println("重建文件，共 {} 条链条...", survivors.size());

        // 5.1 按完整符号信息分组（包含区段）
        struct SymKey
        {
            std::string name;
            int segment; // ← 区段索引
            uint8_t sourceMode;
            bool isBss;     // ← BSS 标记
            uint64_t start; // 段起始地址
            uint64_t manualBase, arrayBase, arrayIndex;

            bool operator<(const SymKey &o) const
            {
                return std::tie(name, segment, sourceMode, isBss, start,
                                manualBase, arrayBase, arrayIndex) <
                       std::tie(o.name, o.segment, o.sourceMode, o.isBss, o.start,
                                o.manualBase, o.arrayBase, o.arrayIndex);
            }
        };

        std::map<SymKey, std::vector<const ChainData *>> groups;
        int maxLevel = 0;
        for (const auto &[fp, cd] : survivors)
        {
            SymKey sk;
            sk.name = cd.sym.name;
            sk.segment = cd.sym.segment; // ← 区段
            sk.sourceMode = cd.sym.sourceMode;
            sk.isBss = cd.sym.isBss; // ← BSS
            sk.start = cd.sym.start;
            sk.manualBase = cd.sym.manualBase;
            sk.arrayBase = cd.sym.arrayBase;
            sk.arrayIndex = cd.sym.arrayIndex;
            groups[sk].push_back(&cd);
            maxLevel = std::max(maxLevel, cd.sym.level);
        }

        // 5.2 构建树并扁平化
        struct TreeNode
        {
            PtrDir data;
            std::map<int64_t, TreeNode *> children;
            ~TreeNode()
            {
                for (auto &[k, v] : children)
                    delete v;
            }
        };

        std::vector<std::vector<PtrDir>> outLevels(maxLevel > 0 ? maxLevel : 1);

        struct OutBlock
        {
            BinSym sym;
            std::vector<PtrDir> roots;
        };
        std::vector<OutBlock> outBlocks;

        for (const auto &[sk, chains] : groups)
        {
            // 用第一条链的 sym 做模板（包含完整的区段信息）
            BinSym sym = chains[0]->sym;

            // 按 (address, value) 合并相同根
            struct RootKey
            {
                uintptr_t addr, val;
                bool operator<(const RootKey &o) const
                {
                    return std::tie(addr, val) < std::tie(o.addr, o.val);
                }
            };
            std::map<RootKey, TreeNode *> rootMap;

            for (const auto *cd : chains)
            {
                RootKey rk{cd->root.address, cd->root.value};
                TreeNode *node;
                auto it = rootMap.find(rk);
                if (it == rootMap.end())
                {
                    node = new TreeNode();
                    node->data = cd->root;
                    rootMap[rk] = node;
                }
                else
                {
                    node = it->second;
                }

                // 沿 path 插入（path 不含根自身）
                TreeNode *cur = node;
                for (const auto &pn : cd->path)
                {
                    int64_t off = (int64_t)pn.address - (int64_t)cur->data.value;
                    auto cit = cur->children.find(off);
                    if (cit == cur->children.end())
                    {
                        auto *nn = new TreeNode();
                        nn->data = pn;
                        cur->children[off] = nn;
                        cur = nn;
                    }
                    else
                    {
                        cur = cit->second;
                    }
                }
            }

            // 扁平化：根在 maxLevel 层，子节点在 maxLevel-1 ... 0
            std::function<void(TreeNode *, int)> flatten;
            flatten = [&](TreeNode *n, int nodeLevel)
            {
                if (n->children.empty() || nodeLevel <= 0)
                {
                    n->data.start = 0;
                    n->data.end = 0;
                    return;
                }

                int childLevel = nodeLevel - 1;
                if (childLevel >= (int)outLevels.size())
                {
                    n->data.start = 0;
                    n->data.end = 0;
                    return;
                }

                auto &vec = outLevels[childLevel];
                n->data.start = (uint32_t)vec.size();
                for (auto &[off, child] : n->children)
                {
                    flatten(child, childLevel);
                    vec.push_back(child->data);
                }
                n->data.end = (uint32_t)vec.size();
            };

            std::vector<PtrDir> finalRoots;
            for (auto &[rk, rn] : rootMap)
            {
                flatten(rn, maxLevel);
                finalRoots.push_back(rn->data);
                delete rn;
            }
            rootMap.clear();

            sym.pointer_count = (int)finalRoots.size();
            sym.level = maxLevel;
            outBlocks.push_back({sym, std::move(finalRoots)});
        }

        // ===== 6. 写入磁盘 =====
        FILE *fOut = fopen("Pointer_Merged.tmp", "wb");
        if (!fOut)
        {
            std::println(stderr, "无法创建输出文件");
            return;
        }

        BinHeader hdr{};
        strcpy(hdr.sign, ".bin pointer chain");
        hdr.size = sizeof(uintptr_t);
        hdr.version = 102;
        hdr.module_count = (int)outBlocks.size();
        hdr.level = maxLevel;

        const auto &first = survivors.begin()->second;
        hdr.scanBaseMode = first.sym.sourceMode;
        hdr.scanManualBase = first.sym.manualBase;
        hdr.scanArrayBase = first.sym.arrayBase;
        hdr.scanTarget = (!first.path.empty()) ? first.path.back().value : first.root.value;

        fwrite(&hdr, sizeof(hdr), 1, fOut);

        // 符号块 + 根节点（完整保留区段信息）
        for (const auto &b : outBlocks)
        {
            fwrite(&b.sym, sizeof(BinSym), 1, fOut);
            fwrite(b.roots.data(), sizeof(PtrDir), b.roots.size(), fOut);
        }

        // 层级节点
        for (int i = 0; i < maxLevel; ++i)
        {
            BinLevel bl;
            bl.level = i;
            bl.count = (uint32_t)outLevels[i].size();
            fwrite(&bl, sizeof(bl), 1, fOut);
            if (bl.count > 0)
                fwrite(outLevels[i].data(), sizeof(PtrDir), bl.count, fOut);
        }

        fflush(fOut);
        fclose(fOut);

        for (const auto &fn : files)
            remove(fn.c_str());
        rename("Pointer_Merged.tmp", "Pointer.bin");

        std::println("合并完成！{} 条链条 → Pointer.bin", survivors.size());
    }

    void ExportToTxt()
    {
        std::println("=== 导出文本链条 ===");

        const char *binFile = "Pointer.bin";
        const char *txtFile = "Pointer_Export.txt";

        FILE *f = fopen(binFile, "rb");
        if (!f)
        {
            std::println(stderr, "无法打开 {}", binFile);
            return;
        }

        fseek(f, 0, SEEK_END);
        long fileSize = ftell(f);
        rewind(f);
        std::vector<char> buf(fileSize);
        if ((long)fread(buf.data(), 1, fileSize, f) != fileSize)
        {
            fclose(f);
            std::println(stderr, "读取失败");
            return;
        }
        fclose(f);

        char *cur = buf.data();
        char *eof = buf.data() + fileSize;

        if (cur + (long)sizeof(BinHeader) > eof)
        {
            std::println(stderr, "文件头不完整");
            return;
        }
        BinHeader *hdr = (BinHeader *)cur;
        cur += sizeof(BinHeader);

        // 解析符号块
        struct SymBlock
        {
            BinSym sym;
            std::vector<PtrDir> roots;
        };
        std::vector<SymBlock> blocks;

        for (int i = 0; i < hdr->module_count; ++i)
        {
            if (cur + (long)sizeof(BinSym) > eof)
                break;
            BinSym *s = (BinSym *)cur;
            cur += sizeof(BinSym);

            long need = s->pointer_count * (long)sizeof(PtrDir);
            if (cur + need > eof)
                break;

            std::vector<PtrDir> roots(s->pointer_count);
            memcpy(roots.data(), cur, need);
            cur += need;
            blocks.push_back({*s, std::move(roots)});
        }

        // 解析层级
        int lvlCount = hdr->level + 1;
        std::vector<std::vector<PtrDir>> levels(lvlCount > 0 ? lvlCount : 1);
        while (cur + (long)sizeof(BinLevel) <= eof)
        {
            BinLevel *bl = (BinLevel *)cur;
            cur += sizeof(BinLevel);

            if (bl->level < 0 || bl->level >= (int)levels.size())
                break;

            long need = bl->count * (long)sizeof(PtrDir);
            if (cur + need > eof)
                break;

            levels[bl->level].resize(bl->count);
            memcpy(levels[bl->level].data(), cur, need);
            cur += need;
        }

        FILE *fOut = fopen(txtFile, "w");
        if (!fOut)
        {
            std::println(stderr, "无法创建 {}", txtFile);
            return;
        }

        fprintf(fOut, "// Pointer Scan Export\n");
        fprintf(fOut, "// Version: %d, Depth: %d\n", hdr->version, hdr->level);
        fprintf(fOut, "// Target: 0x%llX\n", (unsigned long long)hdr->scanTarget);
        fprintf(fOut, "// Base Mode: %d (0=Module, 1=Manual, 2=Array)\n", hdr->scanBaseMode);
        fprintf(fOut, "// ========================================\n\n");

        size_t chainCount = 0;
        std::vector<int64_t> offsets;

        // DFS 递归
        std::function<void(int, const PtrDir &)> dfs;
        dfs = [&](int lvl, const PtrDir &node)
        {
            bool leaf = (lvl < 0) ||
                        (lvl >= (int)levels.size()) ||
                        (node.start >= node.end);

            if (leaf)
            {
                for (auto off : offsets)
                {
                    if (off >= 0)
                        fprintf(fOut, " + 0x%lX", (unsigned long)off);
                    else
                        fprintf(fOut, " - 0x%lX", (unsigned long)(-off));
                }
                fprintf(fOut, "\n");
                chainCount++;
                return;
            }

            const auto &layer = levels[lvl];
            for (uint32_t i = node.start; i < node.end && i < (uint32_t)layer.size(); ++i)
            {
                const PtrDir &child = layer[i];
                int64_t off = (int64_t)child.address - (int64_t)node.value;
                offsets.push_back(off);
                dfs(lvl - 1, child);
                offsets.pop_back();
            }
        };

        // 遍历所有符号块
        for (const auto &blk : blocks)
        {
            // ===== 构建基址显示名 =====
            // 格式: "模块名[区段索引]" 或 "Manual" 或 "Array[N]"
            char baseStr[256];
            uint64_t baseAddr;

            switch (blk.sym.sourceMode)
            {
            case 1: // 手动模式
                snprintf(baseStr, sizeof(baseStr), "\"Manual_0x%llX\"",
                         (unsigned long long)blk.sym.manualBase);
                baseAddr = blk.sym.manualBase;
                break;

            case 2: // 数组模式
                snprintf(baseStr, sizeof(baseStr), "\"Array[%llu]\"",
                         (unsigned long long)blk.sym.arrayIndex);
                baseAddr = blk.sym.start; // 对象地址
                break;

            default: // 模块模式: 显示 "libc.so[2]" 或 "libc.so[-1]"
                snprintf(baseStr, sizeof(baseStr), "\"%s[%d]\"",
                         blk.sym.name, blk.sym.segment);
                baseAddr = blk.sym.start; // 段起始地址
                break;
            }

            // 遍历该符号的所有根节点
            for (const auto &root : blk.roots)
            {
                int64_t rootOff = (int64_t)root.address - (int64_t)baseAddr;

                // 行首: ["libc.so[2]" + 0x1234]
                fprintf(fOut, "[%s", baseStr);
                if (rootOff >= 0)
                    fprintf(fOut, " + 0x%lX]", (unsigned long)rootOff);
                else
                    fprintf(fOut, " - 0x%lX]", (unsigned long)(-rootOff));

                offsets.clear();
                dfs(blk.sym.level - 1, root);
            }
        }

        fclose(fOut);
        std::println("导出完成: {} 条链条 → {}", chainCount, txtFile);
    }
};

// ============================================================================
// 锁定管理器 (Lock Manager)
// ============================================================================
class LockManager
{
private:
    struct LockItem
    {
        uintptr_t addr;
        Types::DataType type;
        std::string value;
    };
    std::list<LockItem> locks_;
    mutable std::mutex mutex_;
    std::jthread writeThread_;
    auto find(uintptr_t addr)
    {
        return std::ranges::find_if(locks_, [addr](auto &i)
                                    { return i.addr == addr; });
    }
    void writeLoop(std::stop_token stoken)
    {
        while (!stoken.stop_requested() && Config::g_Running)
        {
            {
                std::lock_guard lock(mutex_);
                for (auto &item : locks_)
                {
                    MemUtils::WriteFromString(item.addr, item.type, item.value);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

public:
    LockManager()
        : writeThread_([this](std::stop_token st)
                       { writeLoop(st); })
    {
    }

    ~LockManager()
    {
        writeThread_.request_stop();
    }

    bool isLocked(uintptr_t addr) const
    {
        std::lock_guard lock(mutex_);
        return std::ranges::any_of(locks_, [addr](const auto &i)
                                   { return i.addr == addr; });
    }
    void toggle(uintptr_t addr, Types::DataType type)
    {
        std::lock_guard lock(mutex_);
        auto it = find(addr);
        if (it != locks_.end())
            locks_.erase(it);
        else
            locks_.push_back({addr, type, MemUtils::ReadAsString(addr, type)});
    }
    void lock(uintptr_t addr, Types::DataType type, const std::string &value)
    {
        std::lock_guard lk(mutex_);
        if (find(addr) == locks_.end())
            locks_.push_back({addr, type, value});
    }
    void unlock(uintptr_t addr)
    {
        std::lock_guard lk(mutex_);
        std::erase_if(locks_,
                      [addr](const auto &item)
                      { return item.addr == addr; });
    }

    void lockBatch(std::span<const uintptr_t> addrs, Types::DataType type)
    {
        std::lock_guard lk(mutex_);
        for (auto addr : addrs)
        {
            if (!std::ranges::any_of(locks_,
                                     [addr](const auto &item)
                                     { return item.addr == addr; }))
            {
                locks_.emplace_back(addr, type,
                                    MemUtils::ReadAsString(addr, type));
            }
        }
    }

    void unlockBatch(std::span<const uintptr_t> addrs)
    {
        std::lock_guard lk(mutex_);
        for (auto addr : addrs)
        {
            std::erase_if(locks_,
                          [addr](const auto &item)
                          { return item.addr == addr; });
        }
    }

    void clear()
    {
        std::lock_guard lk(mutex_);
        locks_.clear();
    }
};
// ============================================================================
// 内存浏览器 (Memory Viewer)
// ============================================================================
class MemViewer
{
private:
    uintptr_t base_ = 0;
    Types::ViewFormat format_ = Types::ViewFormat::Hex;
    std::vector<uint8_t> buffer_;
    bool visible_ = false;
    bool readSuccess_ = false;
    Disasm::Disassembler disasm_;
    std::vector<Disasm::DisasmLine> disasmCache_;
    int disasmScrollIdx_ = 0;

public:
    MemViewer() : buffer_(Config::Constants::MEM_VIEW_RANGE * 2) {}

    bool isVisible() const noexcept { return visible_; }
    void setVisible(bool v) noexcept { visible_ = v; }
    Types::ViewFormat format() const noexcept { return format_; }
    bool readSuccess() const noexcept { return readSuccess_; }
    uintptr_t base() const noexcept { return base_; }
    const std::vector<uint8_t> &buffer() const noexcept { return buffer_; }
    const std::vector<Disasm::DisasmLine> &getDisasm() const noexcept { return disasmCache_; }
    int disasmScrollIdx() const noexcept { return disasmScrollIdx_; }

    void setFormat(Types::ViewFormat fmt)
    {
        format_ = fmt;
        disasmScrollIdx_ = 0;
        refresh();
    }

    void open(uintptr_t addr)
    {
        if (format_ == Types::ViewFormat::Disasm)
            addr &= ~static_cast<uintptr_t>(3);
        base_ = addr;
        disasmScrollIdx_ = 0;
        refresh();
        visible_ = true;
    }

    void move(int lines, size_t step)
    {
        if (format_ == Types::ViewFormat::Disasm)
        {
            moveDisasm(lines);
        }
        else
        {
            int64_t delta = static_cast<int64_t>(lines) * static_cast<int64_t>(step);
            if (delta < 0 && base_ < static_cast<uintptr_t>(-delta))
                base_ = 0;
            else
                base_ += delta;
            refresh();
        }
    }

    void refresh()
    {
        if (base_ > Config::Constants::ADDR_MAX)
        {
            readSuccess_ = false;
            disasmCache_.clear();
            return;
        }
        std::ranges::fill(buffer_, 0);
        readSuccess_ = (dr.Read(base_, buffer_.data(), buffer_.size()) == 0);
        if (!readSuccess_)
        {
            disasmCache_.clear();
            return;
        }
        if (format_ == Types::ViewFormat::Disasm)
        {
            disasmCache_.clear();
            disasmScrollIdx_ = 0;
            if (disasm_.IsValid() && !buffer_.empty())
                disasmCache_ = disasm_.Disassemble(base_, buffer_.data(), buffer_.size(), 0);
        }
    }
    bool applyOffset(std::string_view offsetStr)
    {
        auto result = MemUtils::ParseHexOffset(offsetStr);
        if (!result)
            return false;
        uintptr_t newBase = result->negative ? (base_ - result->offset) : (base_ + result->offset);
        open(newBase);
        return true;
    }

private:
    // 缓存内滚动不触发 refresh()
    void moveDisasm(int lines)
    {
        if (lines == 0)
            return;

        // 无缓存时直接移动 base_ 并读取
        if (disasmCache_.empty())
        {
            if (lines > 0)
                base_ += lines * 4;
            else
            {
                size_t back = static_cast<size_t>(-lines) * 4;
                base_ = (base_ > back) ? (base_ - back) : 0;
            }
            base_ &= ~static_cast<uintptr_t>(3);
            disasmScrollIdx_ = 0;
            refresh();
            return;
        }

        int newIdx = disasmScrollIdx_ + lines;
        int maxIdx = static_cast<int>(disasmCache_.size());

        // 预留50行缓冲，屏幕约显示30行，确保不会显示到缓存外
        static constexpr int MARGIN = 50;

        if (newIdx < 0)
        {
            // ★ 超出缓存上方：base_ 前移，重新读取
            size_t back = static_cast<size_t>(-newIdx) * 4;
            base_ = (base_ > back) ? (base_ - back) : 0;
            base_ &= ~static_cast<uintptr_t>(3);
            disasmScrollIdx_ = 0;
            refresh();
        }
        else if (newIdx + MARGIN >= maxIdx)
        {
            // ★ 接近缓存底部：以目标行地址为新 base_，重新读取
            size_t targetIdx = std::min(static_cast<size_t>(newIdx),
                                        disasmCache_.size() - 1);
            base_ = disasmCache_[targetIdx].address;
            disasmScrollIdx_ = 0;
            refresh();
        }
        else
        {

            disasmScrollIdx_ = newIdx;
        }
    }
};

// ============================================================================
// 应用上下文 (Application Context)
// ============================================================================
class AppContext
{
public:
    MemScanner scanner;
    PointerManager ptrManager;
    LockManager lockManager;
    MemViewer memViewer;

    struct ScanParams
    {
        Types::DataType dataType = Types::DataType::I32;
        Types::FuzzyMode fuzzyMode = Types::FuzzyMode::Unknown;
        int page = 0;
    } scanParams;

    struct PtrParams
    {
        uintptr_t target = 0;
        int depth = 3;
        int maxOffset = 1000;
        bool useManual = false;
        uintptr_t manualBase = 0;
        bool useArray = false;
        uintptr_t arrayBase = 0;
        size_t arrayCount = 0;
        std::string filterModule;
    } ptrParams;

    struct SigParams
    {
        uintptr_t scanAddr = 0;
        int range = 20;
        uintptr_t verifyAddr = 0;
        int lastChanged = -1;
        int lastTotal = 0;
        int lastScanCount = -1;
    } sigParams;

    std::vector<std::string> offsetLabels;
    std::vector<int> offsetValues;
    int selectedOffsetIdx = 1;

    AppContext()
    {
        for (int i = 500; i <= 100000; i += 500)
        {
            offsetLabels.push_back(std::to_string(i));
            offsetValues.push_back(i);
        }
    }

    void startScan(std::string_view valueStr, bool isFirst)
    {
        scanParams.page = 0;
        auto type = scanParams.dataType;
        auto mode = scanParams.fuzzyMode;
        auto pid = dr.GetGlobalPid();
        std::string valCopy(valueStr);
        double rangeMax = 0.0;

        if (mode == Types::FuzzyMode::Range)
        {
            auto pos = valCopy.find('~');
            if (pos == std::string::npos)
                return;
            try
            {
                rangeMax = std::stod(valCopy.substr(pos + 1));
                valCopy = valCopy.substr(0, pos);
            }
            catch (...)
            {
                return;
            }
        }

        Utils::GlobalPool.push([=, this]
                               {
        try {
            MemUtils::DispatchType(type, [&]<typename T>(){
                T val;
                if constexpr (std::is_floating_point_v<T>)
                    val = static_cast<T>(std::stod(valCopy));
                else if constexpr (sizeof(T) <= 4)
                    val = static_cast<T>(std::stoi(valCopy));
                else
                    val = static_cast<T>(std::stoll(valCopy));
                scanner.scan<T>(pid, val, mode, isFirst, rangeMax);
            });
        } catch (...) {} });
    }

    void startPtrScan()
    {
        auto params = ptrParams;
        params.maxOffset = offsetValues[selectedOffsetIdx];
        auto pid = dr.GetGlobalPid();
        Utils::GlobalPool.push([=, this]
                               { ptrManager.scan(pid, params.target, params.depth,
                                                 params.maxOffset, params.useManual, params.manualBase,
                                                 params.maxOffset, params.useArray, params.arrayBase,
                                                 params.arrayCount, params.filterModule); });
    }

    int getMaxOffset() const { return offsetValues[selectedOffsetIdx]; }
};

inline AppContext &App()
{
    static AppContext instance;
    return instance;
}

// ============================================================================
// UI 辅助工具与样式
// ============================================================================
class UIStyle
{
public:
    float scale = 2.0f;
    float margin = 40.0f;
    constexpr float S(float v) const noexcept { return v * scale; }

    void apply() const
    {
        ImGuiStyle &s = ImGui::GetStyle();
        s.FramePadding = {S(10), S(10)};
        s.ItemSpacing = {S(6), S(6)};
        s.TouchExtraPadding = {8, 8};
        s.ScrollbarSize = S(22);
        s.GrabMinSize = S(18);
        s.WindowRounding = S(8);
        s.ChildRounding = S(6);
        s.FrameRounding = S(5);
        s.WindowPadding = {S(8), S(8)};
        s.WindowBorderSize = 0;
    }
};

namespace UI
{
    inline void Space(float y) { ImGui::Dummy({0, y}); }
    inline void Text(ImVec4 col, const char *fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        ImGui::TextColoredV(col, fmt, args);
        va_end(args);
    }
    inline bool Btn(const char *label, const ImVec2 &size, ImVec4 col = {})
    {
        if (col.w > 0)
            ImGui::PushStyleColor(ImGuiCol_Button, col);
        bool res = ImGui::Button(label, size);
        if (col.w > 0)
            ImGui::PopStyleColor();
        return res;
    }
    inline bool KbBtn(const char *text, const char *emptyTxt, const ImVec2 &size, char *targetBuf, int maxLen, const char *title)
    {
        // 使用指针地址作为绝对稳定的 ID，防止按键文字改变后焦点丢失
        ImGui::PushID(static_cast<const void *>(targetBuf));
        bool res = false;
        if (ImGui::Button(strlen(text) ? text : emptyTxt, size))
        {
            ImGuiFloatingKeyboard::Open(targetBuf, maxLen, title);
            res = true;
        }
        ImGui::PopID();
        return res;
    }
}

// ============================================================================
// 主界面 (Main UI)
// ============================================================================
class MainUI
{
private:
    UIStyle style_;
    struct Buffers
    {
        char pid[32] = "", value[64] = "", addAddr[32] = "", base[32] = "", page[16] = "20";
        char modify[64] = "", memOffset[32] = "", resultOffset[32] = "", moduleSearch[64] = "";
        char ptrTarget[32] = "", arrayBase[32] = "", arrayCount[16] = "100", filterModule[64] = "";
        char sigScanAddr[32] = "", sigVerifyAddr[32] = "";
    } buf_;

    struct UIState
    {
        int tab = 0, resultScrollIdx = 0;
        uintptr_t modifyAddr = 0;
        bool showModify = false, floating = false, dragging = false;
        ImVec2 floatPos = {50, 200}, dragOffset = {0, 0};
        bool showType = false, showMode = false, showDepth = false, showOffset = false, showScale = false, showFormat = false;
    } state_;

    float S(float v) const { return style_.S(v); }

public:
    MainUI()
    {
        snprintf(buf_.page, sizeof(buf_.page), "%d", Config::g_ItemsPerPage.load());
        SetInputBlocking(true);
    }

    void draw()
    {
        float sw = RenderVK::displayInfo.width, sh = RenderVK::displayInfo.height;
        style_.apply();
        if (state_.floating)
            drawFloatButton(sw, sh);
        else
        {
            float x = style_.margin, y = style_.margin, w = sw - 2 * x, h = sh - 2 * y;
            drawMainWindow(x, y, w, h);
            drawPopups(x, y, w, h);
        }
        ImGuiFloatingKeyboard::Draw();
    }

private:
    void drawFloatButton(float sw, float sh)
    {
        float sz = S(65);
        state_.floatPos.x = std::clamp(state_.floatPos.x, style_.margin, sw - sz - style_.margin);
        state_.floatPos.y = std::clamp(state_.floatPos.y, style_.margin, sh - sz - style_.margin);
        ImGui::SetNextWindowPos(state_.floatPos);
        ImGui::SetNextWindowSize({sz, sz});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, sz / 2);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
        ImGui::PushStyleColor(ImGuiCol_WindowBg, {0.2f, 0.5f, 0.8f, 0.9f});
        if (ImGui::Begin("##Float", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove))
        {
            auto mpos = ImGui::GetIO().MousePos;
            auto wpos = ImGui::GetWindowPos();
            bool hover = ImGui::IsWindowHovered(), down = ImGui::GetIO().MouseDown[0];
            if (hover && down && !state_.dragging)
            {
                state_.dragging = true;
                state_.dragOffset = {mpos.x - wpos.x, mpos.y - wpos.y};
            }

            if (state_.dragging)
            {
                if (down)
                    state_.floatPos = {mpos.x - state_.dragOffset.x, mpos.y - state_.dragOffset.y};
                else
                    state_.dragging = false;
            }

            if (ImGui::Button("M", {sz, sz}) && !state_.dragging)
            {
                state_.floating = false;
                SetInputBlocking(true);
            }
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
    }

    void drawMainWindow(float x, float y, float w, float h)
    {
        ImGui::SetNextWindowPos({x, y});
        ImGui::SetNextWindowSize({w, h});
        ImGui::PushStyleColor(ImGuiCol_WindowBg, {0.06f, 0.06f, 0.08f, 1.0f});
        if (ImGui::Begin("##Main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove))
        {
            float cw = ImGui::GetContentRegionAvail().x, ch = ImGui::GetContentRegionAvail().y;
            drawTopBar(cw, S(55));
            UI::Space(S(4));
            drawContent(cw, ch - S(55) - S(60) - S(12));
            UI::Space(S(4));
            drawTabs(cw, S(60));
        }
        ImGui::End();
        ImGui::PopStyleColor();
    }

    void drawTopBar(float w, float h)
    {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.1f, 0.1f, 0.12f, 1.0f});
        if (ImGui::BeginChild("Top", {w, h}, true, ImGuiWindowFlags_NoScrollbar))
        {
            float bh = h - S(12);
            if (UI::Btn("收起", {S(55), bh}, {0.15f, 0.4f, 0.6f, 1.0f}))
            {
                state_.floating = true;
                SetInputBlocking(false);
            }
            ImGui::SameLine();
            ImGui::SetCursorPosX((w - ImGui::CalcTextSize("内存扫描").x) / 2);
            ImGui::SetCursorPosY((h - ImGui::GetTextLineHeight()) / 2);
            ImGui::Text("内存扫描");
            ImGui::SameLine(w - (S(50) + S(85) + S(50) + S(50) + S(18)));
            ImGui::SetCursorPosY(S(6));
            char sc[16];
            snprintf(sc, sizeof(sc), "%.0f%%", style_.scale * 100);
            if (ImGui::Button(sc, {S(50), bh}))
                state_.showScale = !state_.showScale;
            ImGui::SameLine();
            UI::KbBtn(buf_.pid, "PID", {S(85), bh}, buf_.pid, 31, "PID");
            ImGui::SameLine();
            if (UI::Btn("同步", {S(50), bh}, {0.18f, 0.4f, 0.22f, 1.0f}))
                dr.SetGlobalPid(atoi(buf_.pid));
            ImGui::SameLine();
            if (UI::Btn("退出", {S(50), bh}, {0.65f, 0.15f, 0.15f, 1.0f}))
                Config::g_Running = false;
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    void drawContent(float w, float h)
    {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.08f, 0.08f, 0.1f, 1.0f});
        if (ImGui::BeginChild("Content", {w, h}, true))
        {
            switch (state_.tab)
            {
            case 0:
                drawScanTab();
                break;
            case 1:
                drawResultTab();
                break;
            case 2:
                drawModuleTab();
                break;
            case 3:
                drawPointerTab();
                break;
            case 4:
                drawSignatureTab();
                break;
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    void drawScanTab()
    {
        float w = ImGui::GetContentRegionAvail().x, bh = S(45);
        auto &params = App().scanParams;
        UI::Text({0.6f, 0.6f, 0.65f, 1}, "数据类型:");
        if (ImGui::Button(Types::Labels::TYPE[static_cast<int>(params.dataType)], {w, bh}))
            state_.showType = true;
        UI::Space(S(6));
        UI::Text({0.6f, 0.6f, 0.65f, 1}, "搜索模式:");
        if (ImGui::Button(Types::Labels::FUZZY[static_cast<int>(params.fuzzyMode)], {w, bh}))
            state_.showMode = true;
        UI::Space(S(6));
        UI::Text({0.6f, 0.6f, 0.65f, 1}, "搜索数值:");
        UI::KbBtn(buf_.value, "点击输入...", {w, S(52)}, buf_.value, 63, "数值");
        if (params.fuzzyMode == Types::FuzzyMode::Range)
            UI::Text({0.4f, 0.8f, 1.0f, 1}, "格式: 最小值~最大值  例: 0~45  -2~2  0.1~6.5");

        UI::Space(S(10));
        float bw = (w - S(12)) / 3;
        ImGui::BeginDisabled(App().scanner.isScanning());
        if (UI::Btn("首次扫描", {bw, S(52)}, {0.12f, 0.38f, 0.18f, 1.0f}))
            App().startScan(buf_.value, true);
        ImGui::SameLine();
        if (UI::Btn("再次扫描", {bw, S(52)}, {0.12f, 0.25f, 0.4f, 1.0f}))
            App().startScan(buf_.value, false);
        ImGui::SameLine();
        if (UI::Btn("清空", {bw, S(52)}, {0.38f, 0.15f, 0.15f, 1.0f}))
            App().scanner.clear();
        ImGui::EndDisabled();

        UI::Space(S(6));
        if (App().scanner.isScanning())
        {
            UI::Text({1, 0.8f, 0.2f, 1}, "扫描中...");
            ImGui::ProgressBar(App().scanner.progress(), {w, S(18)});
        }
        else
        {
            size_t cnt = App().scanner.count();
            cnt ? UI::Text({0.4f, 0.9f, 0.4f, 1}, "找到 %zu 个", cnt) : UI::Text({0.5f, 0.5f, 0.5f, 1}, "暂无结果");
        }
    }

    void drawResultTab()
    {
        auto &app = App();
        size_t total = app.scanner.count();
        float w = ImGui::GetContentRegionAvail().x, bh = S(40), addW = S(70);
        UI::KbBtn(buf_.addAddr, "Hex地址...", {w - addW - S(6), bh}, buf_.addAddr, 31, "Hex地址");
        ImGui::SameLine();
        if (UI::Btn("添加", {addW, bh}, {0.12f, 0.38f, 0.22f, 1.0f}))
        {
            uintptr_t addr = 0;
            if (sscanf(buf_.addAddr, "%lx", &addr) == 1 && addr)
            {
                app.scanner.add(addr);
                buf_.addAddr[0] = 0;
            }
        }
        if (total == 0)
        {
            UI::Text({0.5f, 0.5f, 0.5f, 1}, "暂无结果");
            return;
        }

        int perPage = Config::g_ItemsPerPage.load(), maxPage = static_cast<int>((total - 1) / perPage);
        app.scanParams.page = std::clamp(app.scanParams.page, 0, maxPage);
        auto data = app.scanner.getPage(app.scanParams.page * perPage, perPage);

        UI::Space(S(4));
        float pgW = S(65);
        ImGui::BeginDisabled(app.scanParams.page <= 0);
        if (ImGui::Button("上页", {pgW, bh}))
        {
            --app.scanParams.page;
            state_.resultScrollIdx = 0;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();

        char info[64];
        snprintf(info, sizeof(info), "%d/%d (共%zu)", app.scanParams.page + 1, maxPage + 1, total);
        float infoW = w - pgW * 2 - S(12);

        ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.1f, 0.1f, 0.12f, 1.0f});
        if (ImGui::BeginChild("PageInfo", {infoW, bh}, true, ImGuiWindowFlags_NoScrollbar))
        {
            ImGui::SetCursorPos({(infoW - ImGui::CalcTextSize(info).x) / 2, (bh - ImGui::GetTextLineHeight()) / 2 - S(4)});
            ImGui::Text("%s", info);
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::SameLine();

        ImGui::BeginDisabled(app.scanParams.page >= maxPage);
        if (ImGui::Button("下页", {pgW, bh}))
        {
            ++app.scanParams.page;
            state_.resultScrollIdx = 0;
        }
        ImGui::EndDisabled();

        UI::Space(S(4));
        ImGui::Text("每页:");
        ImGui::SameLine();
        UI::KbBtn(buf_.page, buf_.page, {S(55), S(36)}, buf_.page, 10, "每页数量");
        ImGui::SameLine();
        if (ImGui::Button("应用", {S(50), S(36)}))
        {
            int v = atoi(buf_.page);
            if (v >= 1 && v <= 500)
            {
                Config::g_ItemsPerPage = v;
                app.scanParams.page = state_.resultScrollIdx = 0;
            }
            else
                snprintf(buf_.page, sizeof(buf_.page), "%d", Config::g_ItemsPerPage.load());
        }
        ImGui::SameLine();

        if (std::ranges::any_of(data, [&](auto a)
                                { return app.lockManager.isLocked(a); }))
        {
            if (UI::Btn("解锁页", {S(70), S(36)}, {0.2f, 0.25f, 0.42f, 1.0f}))
                app.lockManager.unlockBatch(data);
        }
        else
        {
            if (UI::Btn("锁定页", {S(70), S(36)}, {0.42f, 0.28f, 0.1f, 1.0f}))
                app.lockManager.lockBatch(data, app.scanParams.dataType);
        }
        ImGui::SameLine();

        if (UI::Btn("偏移", {S(55), S(36)}, {0.35f, 0.25f, 0.15f, 1.0f}))
        {
            buf_.resultOffset[0] = 0;
            ImGuiFloatingKeyboard::Open(buf_.resultOffset, 31, "偏移量(Hex,可负)");
        }
        if (strlen(buf_.resultOffset) > 0 && !ImGuiFloatingKeyboard::IsVisible())
        {
            if (auto r = MemUtils::ParseHexOffset(buf_.resultOffset))
            {
                app.scanner.applyOffset(r->negative ? -static_cast<int64_t>(r->offset) : static_cast<int64_t>(r->offset));
                app.scanParams.page = state_.resultScrollIdx = 0;
            }
            buf_.resultOffset[0] = 0;
        }
        ImGui::Separator();

        float listH = ImGui::GetContentRegionAvail().y, arrowW = S(50), contentW = w - arrowW - S(6);
        int maxIdx = std::max(0, static_cast<int>(data.size()) - static_cast<int>(listH / S(93)));
        state_.resultScrollIdx = std::clamp(state_.resultScrollIdx, 0, maxIdx);

        if (ImGui::BeginChild("ListContent", {contentW, listH}, false, ImGuiWindowFlags_NoScrollbar))
        {
            for (int i = state_.resultScrollIdx; i < static_cast<int>(data.size()) && i < state_.resultScrollIdx + static_cast<int>(listH / S(93)) + 1; ++i)
                drawCard(data[i], contentW - S(10));
        }
        ImGui::EndChild();
        ImGui::SameLine();

        if (ImGui::BeginChild("ListArrows", {arrowW, listH}, false, ImGuiWindowFlags_NoScrollbar))
        {
            float btnH = listH / 2 - S(3);
            ImGui::PushStyleColor(ImGuiCol_Button, {0.2f, 0.3f, 0.4f, 1.0f});
            ImGui::BeginDisabled(state_.resultScrollIdx <= 0);
            if (ImGui::Button("▲##res", {arrowW, btnH}))
                --state_.resultScrollIdx;
            ImGui::EndDisabled();
            ImGui::BeginDisabled(state_.resultScrollIdx >= maxIdx);
            if (ImGui::Button("▼##res", {arrowW, btnH}))
                ++state_.resultScrollIdx;
            ImGui::EndDisabled();
            ImGui::PopStyleColor();
        }
        ImGui::EndChild();
    }

    void drawCard(uintptr_t addr, float w)
    {
        auto &app = App();
        bool locked = app.lockManager.isLocked(addr);
        ImGui::PushID(reinterpret_cast<void *>(addr));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, locked ? ImVec4{0.2f, 0.08f, 0.08f, 1} : ImVec4{0.1f, 0.1f, 0.12f, 1});
        if (ImGui::BeginChild("Card", {w, S(85)}, true, ImGuiWindowFlags_NoScrollbar))
        {
            float cw = ImGui::GetContentRegionAvail().x;
            UI::Text({0.5f, 0.6f, 0.7f, 1}, "地址:");
            ImGui::SameLine();
            UI::Text(locked ? ImVec4{1, 0.5f, 0.5f, 1} : ImVec4{0.5f, 1, 0.5f, 1}, "%lX", addr);
            ImGui::SameLine(cw * 0.45f);
            UI::Text({0.5f, 0.6f, 0.7f, 1}, "数值:");
            ImGui::SameLine();
            UI::Text({1, 1, 0.6f, 1}, "%s", MemUtils::ReadAsString(addr, app.scanParams.dataType).c_str());
            if (locked)
            {
                ImGui::SameLine();
                UI::Text({1, 0.3f, 0.3f, 1}, "[锁定]");
            }
            UI::Space(S(4));
            float bw = (cw - S(15)) / 4, bh = S(36);
            if (ImGui::Button("改", {bw, bh}))
            {
                state_.modifyAddr = addr;
                strcpy(buf_.modify, MemUtils::ReadAsString(addr, app.scanParams.dataType).c_str());
                state_.showModify = true;
                ImGuiFloatingKeyboard::Open(buf_.modify, 63, "新数值");
            }
            ImGui::SameLine();
            if (UI::Btn(locked ? "解锁" : "锁定", {bw, bh}, locked ? ImVec4{0.4f, 0.15f, 0.15f, 1} : ImVec4{0.15f, 0.28f, 0.4f, 1}))
                app.lockManager.toggle(addr, app.scanParams.dataType);
            ImGui::SameLine();
            if (ImGui::Button("浏览", {bw, bh}))
                app.memViewer.open(addr);
            ImGui::SameLine();
            if (UI::Btn("删除", {bw, bh}, {0.4f, 0.1f, 0.1f, 1}))
            {
                if (locked)
                    app.lockManager.unlock(addr);
                app.scanner.remove(addr);
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopID();
        UI::Space(S(4));
    }

    void drawModuleTab()
    {
        float w = ImGui::GetContentRegionAvail().x;

        UI::KbBtn(buf_.moduleSearch, "搜索模块名和dump模块", {w, S(42)}, buf_.moduleSearch, 63, "输入模块名进行搜索或Dump");
        UI::Space(S(4));

        if (UI::Btn("刷新模块", {w, S(48)}, {0.15f, 0.28f, 0.4f, 1.0f}))
            dr.GetMemoryInformation();
        UI::Space(S(6));

        if (UI::Btn("Dump 模块 (保存至 /sdcard/dump/)", {w, S(48)}, {0.35f, 0.25f, 0.45f, 1.0f}))
        {
            if (strlen(buf_.moduleSearch) > 0)
            {
                std::string targetMod = buf_.moduleSearch;
                Utils::GlobalPool.push([targetMod]()
                                       { dr.DumpModule(targetMod); });
            }
        }
        UI::Space(S(6));

        if (ImGui::BeginChild("ModList", {0, 0}, false))
        {
            const auto &info = dr.GetMemoryInfoRef();
            if (info.module_count == 0)
                UI::Text({0.5f, 0.5f, 0.5f, 1}, "暂无模块");
            else
            {
                int displayCount = 0;
                for (int i = 0; i < info.module_count; ++i)
                {
                    const auto &mod = info.modules[i];
                    std::string_view name = mod.name;
                    auto slash = name.rfind('/');
                    if (slash != std::string_view::npos)
                        name = name.substr(slash + 1);

                    if (buf_.moduleSearch[0] && name.find(buf_.moduleSearch) == std::string_view::npos)
                        continue;

                    for (int j = 0; j < mod.seg_count; ++j)
                    {
                        const auto &seg = mod.segs[j];
                        displayCount++;
                        ImGui::PushID(i * 1000 + j);
                        ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.12f, 0.12f, 0.14f, 1.0f});
                        if (ImGui::BeginChild("Mod", {w - S(20), 0}, true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize))
                        {
                            UI::Text({0.7f, 0.85f, 1.0f, 1}, "%.*s", (int)name.size(), name.data());
                            seg.index == -1 ? UI::Text({0.9f, 0.6f, 0.3f, 1}, "Segment: BSS") : UI::Text({0.5f, 0.9f, 0.5f, 1}, "Segment: %d", seg.index);
                            UI::Text({0.5f, 0.5f, 0.5f, 1}, "Range: ");
                            ImGui::SameLine();
                            UI::Text({0.4f, 1.0f, 0.4f, 1}, "%llX - ", (unsigned long long)seg.start);
                            ImGui::SameLine();
                            UI::Text({1.0f, 0.6f, 0.4f, 1}, "%llX", (unsigned long long)seg.end);
                        }
                        ImGui::EndChild();
                        ImGui::PopStyleColor();
                        ImGui::PopID();
                        UI::Space(S(4));
                    }
                }
                if (!displayCount)
                    UI::Text({0.6f, 0.4f, 0.4f, 1.0f}, "未找到匹配 \"%s\" 的模块", buf_.moduleSearch);
            }
        }
        ImGui::EndChild();
    }

    void drawPointerTab()
    {
        auto &app = App();
        auto &params = app.ptrParams;
        float w = ImGui::GetContentRegionAvail().x, bh = S(45);
        ImGui::PushID("PtrScan");
        UI::Text({0.9f, 0.7f, 0.4f, 1}, "━━ 指针扫描 ━━");
        UI::Space(S(4));
        if (!app.ptrManager.isScanning())
        {
            ImGui::Text("目标地址:");
            UI::KbBtn(buf_.ptrTarget, "点击输入Hex", {w, bh}, buf_.ptrTarget, 31, "目标地址(Hex)");
            UI::Space(S(4));
            ImGui::Text("深度:");
            ImGui::SameLine();
            char dLbl[8];
            std::snprintf(dLbl, sizeof(dLbl), "%d层", params.depth);
            if (ImGui::Button(dLbl, {S(70), bh}))
                state_.showDepth = true;
            ImGui::SameLine();
            ImGui::Text("偏移:");
            ImGui::SameLine();
            if (ImGui::Button(app.offsetLabels[app.selectedOffsetIdx].c_str(), {S(70), bh}))
                state_.showOffset = true;
            UI::Space(S(4));
            UI::Text({0.6f, 0.6f, 0.65f, 1}, "指定模块 (可选):");
            UI::KbBtn(buf_.filterModule, "全部模块", {w - S(60), bh}, buf_.filterModule, 63, "模块名(如il2cpp)");
            ImGui::SameLine();
            if (ImGui::Button("清##scanFilter", {S(50), bh}))
                buf_.filterModule[0] = 0;

            ImGui::Checkbox("手动基址##scan", &params.useManual);
            if (params.useManual)
            {
                params.useArray = false;
                UI::KbBtn(buf_.base, "基址(Hex)##scanBase", {w, bh}, buf_.base, 30, "Hex基址");
            }
            ImGui::Checkbox("数组基址##scan", &params.useArray);
            if (params.useArray)
            {
                params.useManual = false;
                float ahw = (w - S(6)) / 2;
                UI::KbBtn(buf_.arrayBase, "数组地址(Hex)##scanArr", {ahw, bh}, buf_.arrayBase, 30, "数组首地址");
                ImGui::SameLine();
                UI::KbBtn(buf_.arrayCount, "数量##scanCnt", {ahw, bh}, buf_.arrayCount, 15, "元素数量");
            }
            UI::Space(S(6));
            if (UI::Btn("开始扫描", {w, S(48)}, {0.15f, 0.4f, 0.25f, 1}))
            {
                if (std::sscanf(buf_.ptrTarget, "%lx", &params.target) == 1 && params.target)
                {
                    params.filterModule = buf_.filterModule;
                    if (params.useManual && buf_.base[0])
                        params.manualBase = std::strtoull(buf_.base, nullptr, 16);
                    if (params.useArray)
                    {
                        if (buf_.arrayBase[0])
                            params.arrayBase = std::strtoull(buf_.arrayBase, nullptr, 16);
                        if (buf_.arrayCount[0])
                            params.arrayCount = std::strtoull(buf_.arrayCount, nullptr, 10);
                    }
                    app.startPtrScan();
                }
            }
            UI::Space(S(12));
            ImGui::Separator();
            UI::Space(S(8));
            UI::Text({0.6f, 0.7f, 0.8f, 1}, "文件操作 (Pointer.bin)");
            UI::Space(S(4));
            float halfW = (w - S(8)) / 2;
            if (UI::Btn("开始对比", {halfW, S(40)}, {0.35f, 0.2f, 0.45f, 1.0f}))
                app.ptrManager.MergeBins();
            ImGui::SameLine();
            if (UI::Btn("格式化输出", {halfW, S(40)}, {0.45f, 0.35f, 0.2f, 1.0f}))
                app.ptrManager.ExportToTxt();

            if (size_t cnt = app.ptrManager.count(); cnt > 0)
            {
                UI::Space(S(6));
                UI::Text({0.4f, 1.0f, 0.4f, 1.0f}, "扫描完成！找到 %zu 条指针链", cnt);
            }
            else if (app.ptrManager.scanProgress() >= 1.0f)
            {
                UI::Space(S(6));
                UI::Text({1.0f, 0.4f, 0.4f, 1.0f}, "扫描完成，未找到结果");
            }
            UI::Text({0.5f, 0.5f, 0.5f, 1}, "保存到 Pointer.bin");
        }
        else
        {
            UI::Text({1, 0.8f, 0.3f, 1}, "扫描中...");
            ImGui::ProgressBar(app.ptrManager.scanProgress(), {w, S(22)});
        }
        ImGui::PopID();
    }

    void drawSignatureTab()
    {
        auto &params = App().sigParams;
        float w = ImGui::GetContentRegionAvail().x, bh = S(45);
        UI::Text({0.9f, 0.7f, 0.4f, 1}, "━━ 特征码扫描 ━━");
        UI::Space(S(4));
        ImGui::Text("目标地址:");
        UI::KbBtn(buf_.sigScanAddr, "点击输入Hex", {w, bh}, buf_.sigScanAddr, 31, "目标地址(Hex)");
        UI::Space(S(4));
        ImGui::Text("范围 (上下各N字节):");
        ImGui::SetNextItemWidth(w);
        ImGui::SliderInt("##sigRange", &params.range, 1, SignatureScanner::SIG_MAX_RANGE, "%d");
        float qbw = (w - S(12)) / 4;
        for (int r : {10, 20, 50, 100})
        {
            char lb[8];
            std::snprintf(lb, sizeof(lb), "%d", r);
            if (ImGui::Button(lb, {qbw, S(30)}))
                params.range = r;
            if (r != 100)
                ImGui::SameLine();
        }
        UI::Space(S(8));

        if (UI::Btn("扫描保存", {w, S(48)}, {0.15f, 0.4f, 0.25f, 1}))
        {
            uintptr_t addr = 0;
            if (std::sscanf(buf_.sigScanAddr, "%lx", &addr) == 1 && addr)
                SignatureScanner::ScanAddressSignature(addr, params.range);
        }
        UI::Text({0.5f, 0.5f, 0.5f, 1}, "保存到 Signature.txt");
        UI::Space(S(20));
        ImGui::Separator();
        UI::Space(S(10));

        UI::Text({0.9f, 0.7f, 0.4f, 1}, "━━ 特征码过滤 ━━");
        UI::Space(S(4));
        ImGui::Text("过滤地址:");
        UI::KbBtn(buf_.sigVerifyAddr, "点击输入Hex", {w, bh}, buf_.sigVerifyAddr, 31, "过滤地址(Hex)");
        UI::Space(S(8));

        if (UI::Btn("过滤并更新", {w, S(48)}, {0.4f, 0.3f, 0.15f, 1}))
        {
            if (std::sscanf(buf_.sigVerifyAddr, "%lx", &params.verifyAddr) == 1 && params.verifyAddr)
            {
                auto vr = SignatureScanner::FilterSignature(params.verifyAddr);
                params.lastChanged = vr.success ? vr.changedCount : -2;
                if (vr.success)
                    params.lastTotal = vr.totalCount;
                params.lastScanCount = -1;
            }
        }
        if (params.lastChanged >= 0)
        {
            params.lastChanged == 0 ? UI::Text({0.4f, 1.0f, 0.4f, 1}, "完美! 无变动 (%d字节)", params.lastTotal)
                                    : UI::Text({1.0f, 0.8f, 0.3f, 1}, "变动: %d/%d (已更新)", params.lastChanged, params.lastTotal);
        }
        else if (params.lastChanged == -2)
            UI::Text({1, 0.4f, 0.4f, 1}, "失败! 检查Signature.txt");

        UI::Space(S(10));
        if (UI::Btn("扫描特征码", {w, S(48)}, {0.3f, 0.2f, 0.45f, 1}))
            params.lastScanCount = static_cast<int>(SignatureScanner::ScanSignatureFromFile().size());

        if (params.lastScanCount >= 0)
            params.lastScanCount == 0 ? UI::Text({1.0f, 0.5f, 0.5f, 1}, "未找到匹配地址") : UI::Text({0.5f, 0.9f, 1.0f, 1}, "找到 %d 个地址", params.lastScanCount);
        UI::Text({0.5f, 0.5f, 0.5f, 1}, "结果保存到 Signature.txt");
    }

    void drawTabs(float w, float h)
    {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.1f, 0.1f, 0.12f, 1.0f});
        if (ImGui::BeginChild("Tabs", {w, h}, true, ImGuiWindowFlags_NoScrollbar))
        {
            float bw = (w - S(30)) / 5, bh = h - S(14);
            const char *l[] = {"扫描", "结果", "模块", "指针", "特征"};
            for (int i = 0; i < 5; ++i)
            {
                if (i > 0)
                    ImGui::SameLine();
                if (UI::Btn(l[i], {bw, bh}, state_.tab == i ? ImVec4{0.2f, 0.32f, 0.5f, 1} : ImVec4{0.12f, 0.12f, 0.15f, 1}))
                {
                    state_.tab = i;
                    if (i == 3 || i == 4)
                        dr.GetMemoryInformation();
                }
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    template <typename F>
    void drawListPopup(const char *title, bool *show, float sx, float sy, float sw, float sh, float pw, float ph, F &&drawItems)
    {
        ImGui::SetNextWindowPos({sx + (sw - pw) / 2, sy + (sh - ph) / 2});
        ImGui::SetNextWindowSize({pw, ph});
        ImGui::PushStyleColor(ImGuiCol_WindowBg, {0.1f, 0.1f, 0.13f, 0.98f});
        if (ImGui::Begin(title, show, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
            drawItems(ImGui::GetContentRegionAvail().x);
        ImGui::End();
        ImGui::PopStyleColor();
    }

    void drawPopups(float sx, float sy, float sw, float sh)
    {
        auto &app = App();
        if (state_.showScale)
        {
            drawListPopup("缩放", &state_.showScale, sx, sy, sw, sh, S(180), S(160), [&](float fw)
                          {
                ImGui::Text("UI: %.0f%%", style_.scale * 100); ImGui::SliderFloat("##s", &style_.scale, 0.5f, 2.0f, "");
                if (ImGui::Button("75%", {fw / 3 - S(3), S(28)})) style_.scale = 0.75f; ImGui::SameLine();
                if (ImGui::Button("100%", {fw / 3 - S(3), S(28)})) style_.scale = 1.0f; ImGui::SameLine();
                if (ImGui::Button("150%", {fw / 3 - S(3), S(28)})) style_.scale = 1.5f;
                ImGui::Text("边距: %.0f", style_.margin); ImGui::SliderFloat("##m", &style_.margin, 0, 80, ""); });
        }
        auto selector = [&](const char *title, bool *show, const char *const *items, int count, int *sel)
        {
            float itemH = S(42);
            drawListPopup(title, show, sx, sy, sw, sh, sw * 0.75f, std::min(count * (itemH + S(4)) + S(50), sh * 0.7f), [&](float fw)
                          {
                for (int i = 0; i < count; ++i) if (UI::Btn(items[i], {fw, itemH}, i == *sel ? ImVec4{0.2f, 0.35f, 0.25f, 1} : ImVec4{0.13f, 0.13f, 0.16f, 1})) { *sel = i; *show = false; } });
        };
        if (state_.showType)
        {
            int sel = static_cast<int>(app.scanParams.dataType);
            selector("类型", &state_.showType, Types::Labels::TYPE.data(), Types::Labels::TYPE.size(), &sel);
            app.scanParams.dataType = static_cast<Types::DataType>(sel);
        }
        if (state_.showMode)
        {
            int sel = static_cast<int>(app.scanParams.fuzzyMode);
            selector("模式", &state_.showMode, Types::Labels::FUZZY.data(), Types::Labels::FUZZY.size(), &sel);
            app.scanParams.fuzzyMode = static_cast<Types::FuzzyMode>(sel);
        }
        if (state_.showFormat)
        {
            int sel = static_cast<int>(app.memViewer.format());
            selector("格式", &state_.showFormat, Types::Labels::FORMAT.data(), static_cast<int>(Types::ViewFormat::Count), &sel);
            app.memViewer.setFormat(static_cast<Types::ViewFormat>(sel));
        }
        if (state_.showDepth)
        {
            drawListPopup("深度", &state_.showDepth, sx, sy, sw, sh, S(160), S(320), [&](float fw)
                          {
                for (int i = 1; i <= 20; ++i) {
                    char lbl[8]; std::snprintf(lbl, sizeof(lbl), "%d层", i);
                    if (UI::Btn(lbl, {fw, S(28)}, i == app.ptrParams.depth ? ImVec4{0.2f, 0.35f, 0.25f, 1} : ImVec4{0.13f, 0.13f, 0.16f, 1})) { app.ptrParams.depth = i; state_.showDepth = false; }
                } });
        }
        if (state_.showOffset)
        {
            drawListPopup("偏移", &state_.showOffset, sx, sy, sw, sh, S(160), std::min(static_cast<float>(app.offsetLabels.size()) * S(32) + S(40), sh * 0.6f), [&](float fw)
                          {
                if (ImGui::BeginChild("List", {0, 0}, false)) {
                    for (size_t i = 0; i < app.offsetLabels.size(); ++i)
                        if (UI::Btn(app.offsetLabels[i].c_str(), {fw, S(28)}, static_cast<int>(i) == app.selectedOffsetIdx ? ImVec4{0.2f, 0.35f, 0.25f, 1} : ImVec4{0.13f, 0.13f, 0.16f, 1})) { app.selectedOffsetIdx = i; state_.showOffset = false; }
                } ImGui::EndChild(); });
        }
        if (app.memViewer.isVisible())
            drawMemViewer(sx, sy, sw, sh);
        if (state_.showModify && !ImGuiFloatingKeyboard::IsVisible())
        {
            if (state_.modifyAddr && strlen(buf_.modify))
                MemUtils::WriteFromString(state_.modifyAddr, app.scanParams.dataType, buf_.modify);
            state_.showModify = false;
            state_.modifyAddr = 0;
            buf_.modify[0] = 0;
        }
    }

    void drawMemViewer(float sx, float sy, float sw, float sh)
    {
        auto &v = App().memViewer;
        ImGui::SetNextWindowPos({sx, sy}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({sw, sh}, ImGuiCond_FirstUseEver);
        bool vis = v.isVisible();
        ImGui::PushStyleColor(ImGuiCol_WindowBg, {0.07f, 0.07f, 0.09f, 0.98f});
        if (ImGui::Begin("内存浏览", &vis, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar))
        {
            float fw = ImGui::GetContentRegionAvail().x, bh = S(38), btnW = S(65), fmtW = S(85);
            ImGui::Text("基址:");
            ImGui::SameLine();
            UI::Text({0.5f, 1, 0.5f, 1}, "%lX", v.base());
            ImGui::SameLine();
            if (!v.readSuccess())
                UI::Text({1.0f, 0.3f, 0.3f, 1.0f}, "[读取失败]");
            ImGui::SameLine(fw - (btnW * 2 + fmtW + S(12)));
            if (ImGui::Button("刷新", {btnW, bh}))
                v.refresh();
            ImGui::SameLine();
            if (UI::Btn("偏移", {btnW, bh}, {0.35f, 0.25f, 0.15f, 1.0f}))
            {
                buf_.memOffset[0] = 0;
                ImGuiFloatingKeyboard::Open(buf_.memOffset, 31, "偏移量(Hex,可负)");
            }
            if (buf_.memOffset[0] && !ImGuiFloatingKeyboard::IsVisible())
            {
                v.applyOffset(buf_.memOffset);
                buf_.memOffset[0] = 0;
            }
            ImGui::SameLine();
            if (UI::Btn(Types::Labels::FORMAT[static_cast<int>(v.format())], {fmtW, bh}, {0.18f, 0.25f, 0.35f, 1.0f}))
                state_.showFormat = true;
            ImGui::Separator();
            if (!v.readSuccess())
            {
                UI::Space(S(20));
                ImGui::PushStyleColor(ImGuiCol_Text, {1.0f, 0.5f, 0.5f, 1.0f});
                ImGui::TextWrapped("无法读取内存，请检查：\n\n1. PID 是否正确并已同步\n2. 目标地址是否有效\n3. 驱动是否正常工作\n4. 目标进程是否仍在运行");
                ImGui::PopStyleColor();
                UI::Space(S(10));
                if (ImGui::Button("重试", {S(80), S(36)}))
                    v.refresh();
            }
            else
            {
                auto fmt = v.format();
                size_t step = fmt == Types::ViewFormat::Disasm ? 1 : (fmt == Types::ViewFormat::Hex ? 4 : Types::GetViewSize(fmt));
                float cH = ImGui::GetContentRegionAvail().y, aW = S(50), cW = ImGui::GetContentRegionAvail().x - aW - S(6);
                float rH = ImGui::GetTextLineHeight() + (fmt == Types::ViewFormat::Disasm ? S(14) : (fmt == Types::ViewFormat::Hex ? S(8) : S(12)));
                int rows = static_cast<int>(cH / rH) + 2;
                if (ImGui::BeginChild("MemContent", {cW, cH}, false, ImGuiWindowFlags_NoScrollbar))
                {
                    if (fmt == Types::ViewFormat::Disasm)
                        drawDisasmView(v.base(), v.getDisasm(), rows, v.disasmScrollIdx());
                    else if (fmt == Types::ViewFormat::Hex)
                        drawHexDump(v.base(), v.buffer(), rows);
                    else
                        drawTypedView(fmt, v.base(), v.buffer(), rows);
                }
                ImGui::EndChild();
                ImGui::SameLine();
                if (ImGui::BeginChild("MemArrows", {aW, cH}, false, ImGuiWindowFlags_NoScrollbar))
                {
                    float bH = cH / 2 - S(3);
                    ImGui::PushStyleColor(ImGuiCol_Button, {0.2f, 0.3f, 0.4f, 1.0f});
                    if (ImGui::Button("▲", {aW, bH}))
                        v.move(-1, step);
                    if (ImGui::Button("▼", {aW, bH}))
                        v.move(1, step);
                    ImGui::PopStyleColor();
                }
                ImGui::EndChild();
            }
        }
        ImGui::End();
        ImGui::PopStyleColor();
        v.setVisible(vis);
    }

    void drawTypedView(Types::ViewFormat format, uintptr_t base, std::span<const uint8_t> buffer, int rows)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {S(6), S(6)});
        if (ImGui::BeginTable("Typed", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("地址", ImGuiTableColumnFlags_WidthFixed, S(100));
            ImGui::TableSetupColumn("数值", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("存", ImGuiTableColumnFlags_WidthFixed, S(50));
            ImGui::TableSetupColumn("跳", ImGuiTableColumnFlags_WidthFixed, S(50));
            ImGui::TableHeadersRow();
            size_t step = Types::GetViewSize(format);
            for (int i = 0; i < rows; ++i)
            {
                size_t off = i * step;
                if (off + step > buffer.size())
                    break;
                uintptr_t addr = base + off;
                const uint8_t *p = buffer.data() + off;
                uint64_t ptrVal = 0;
                ImGui::TableNextRow();
                ImGui::PushID(reinterpret_cast<void *>(addr));
                ImGui::TableSetColumnIndex(0);
                UI::Text(i == 0 ? ImVec4{0.4f, 1, 0.4f, 1} : ImVec4{0.5f, 0.85f, 0.85f, 1}, "%lX", addr);
                ImGui::TableSetColumnIndex(1);
                switch (format)
                {
                case Types::ViewFormat::Hex64:
                    ptrVal = *reinterpret_cast<const uint64_t *>(p);
                    UI::Text({0.6f, 1, 0.6f, 1}, "%lX", ptrVal);
                    break;
                case Types::ViewFormat::I8:
                    ImGui::Text("%d", *reinterpret_cast<const int8_t *>(p));
                    break;
                case Types::ViewFormat::I16:
                    ImGui::Text("%d", *reinterpret_cast<const int16_t *>(p));
                    break;
                case Types::ViewFormat::I32:
                    ptrVal = *reinterpret_cast<const uint32_t *>(p);
                    ImGui::Text("%d", *reinterpret_cast<const int32_t *>(p));
                    break;
                case Types::ViewFormat::I64:
                    ptrVal = *reinterpret_cast<const uint64_t *>(p);
                    ImGui::Text("%lld", (long long)*reinterpret_cast<const int64_t *>(p));
                    break;
                case Types::ViewFormat::Float:
                    ImGui::Text("%.11f", *reinterpret_cast<const float *>(p));
                    break;
                case Types::ViewFormat::Double:
                    ImGui::Text("%.11lf", *reinterpret_cast<const double *>(p));
                    break;
                default:
                    ImGui::Text("?");
                }
                ImGui::TableSetColumnIndex(2);
                if (UI::Btn("存", {S(42), S(28)}, {0.2f, 0.4f, 0.25f, 1}))
                    App().scanner.add(addr);
                ImGui::TableSetColumnIndex(3);
                uintptr_t jump = MemUtils::Normalize(ptrVal);
                bool canJump = (format == Types::ViewFormat::I32 || format == Types::ViewFormat::I64 || format == Types::ViewFormat::Hex64) && MemUtils::IsValidAddr(jump);
                if (canJump)
                {
                    if (UI::Btn("->", {S(42), S(28)}, {0.3f, 0.25f, 0.45f, 1}))
                        App().memViewer.open(jump);
                }
                else
                {
                    ImGui::BeginDisabled();
                    ImGui::Button("-", {S(42), S(28)});
                    ImGui::EndDisabled();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();
    }

    void drawHexDump(uintptr_t base, std::span<const uint8_t> buffer, int rows)
    {
        if (buffer.empty())
        {
            UI::Text({0.5f, 0.5f, 0.5f, 1}, "无数据");
            return;
        }
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {S(3), S(3)});
        if (ImGui::BeginTable("Hex", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("地址", ImGuiTableColumnFlags_WidthFixed, S(85));
            for (int i = 0; i < 4; ++i)
            {
                char h[4];
                std::snprintf(h, sizeof(h), "%X", i);
                ImGui::TableSetupColumn(h, ImGuiTableColumnFlags_WidthFixed, S(24));
            }
            ImGui::TableSetupColumn("ASCII", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("存", ImGuiTableColumnFlags_WidthFixed, S(38));
            ImGui::TableSetupColumn("跳", ImGuiTableColumnFlags_WidthFixed, S(38));
            ImGui::TableHeadersRow();
            for (int i = 0; i < rows; ++i)
            {
                size_t off = i * 4;
                if (off >= buffer.size())
                    break;
                uintptr_t rowAddr = base + off;
                ImGui::TableNextRow();
                ImGui::PushID(reinterpret_cast<void *>(rowAddr));
                ImGui::TableSetColumnIndex(0);
                UI::Text(i == 0 ? ImVec4{0.4f, 1, 0.4f, 1} : ImVec4{0.5f, 0.75f, 0.85f, 1}, "%lX", rowAddr);
                char ascii[5] = {'.', '.', '.', '.', '\0'};
                for (int c = 0; c < 4; ++c)
                {
                    ImGui::TableSetColumnIndex(c + 1);
                    size_t byteOff = off + c;
                    if (byteOff < buffer.size())
                    {
                        uint8_t b = buffer[byteOff];
                        b == 0 ? UI::Text({0.4f, 0.4f, 0.4f, 1}, ".") : ImGui::Text("%02X", b);
                        ascii[c] = (b >= 32 && b < 127) ? static_cast<char>(b) : '.';
                    }
                    else
                    {
                        UI::Text({0.3f, 0.3f, 0.3f, 1}, "??");
                        ascii[c] = ' ';
                    }
                }
                ImGui::TableSetColumnIndex(5);
                UI::Text({0.65f, 0.65f, 0.5f, 1}, "%s", ascii);
                ImGui::TableSetColumnIndex(6);
                if (UI::Btn("存", {S(32), S(22)}, {0.2f, 0.4f, 0.25f, 1}))
                    App().scanner.add(rowAddr);
                ImGui::TableSetColumnIndex(7);

                uintptr_t ptrVal = 0;
                bool canJump = false;
                size_t availBytes = (off < buffer.size()) ? (buffer.size() - off) : 0;
                if (availBytes >= 4)
                {
                    if (availBytes >= 8)
                    {
                        uint64_t raw = 0;
                        std::memcpy(&raw, buffer.data() + off, 8);
                        ptrVal = MemUtils::Normalize(raw);
                        canJump = MemUtils::IsValidAddr(ptrVal);
                    }
                    else
                    {
                        uint32_t raw = 0;
                        std::memcpy(&raw, buffer.data() + off, 4);
                        ptrVal = MemUtils::Normalize(static_cast<uint64_t>(raw));
                        canJump = ptrVal > 0x10000 && ptrVal < 0xFFFFFFFF && ptrVal != 0;
                    }
                }
                if (canJump)
                {
                    if (UI::Btn("->", {S(32), S(22)}, {0.3f, 0.25f, 0.45f, 1}))
                        App().memViewer.open(ptrVal);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("跳转到: %lX", ptrVal);
                }
                else
                {
                    ImGui::BeginDisabled();
                    ImGui::Button("-", {S(32), S(22)});
                    ImGui::EndDisabled();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();
    }

    void drawDisasmView(uintptr_t base, std::span<const Disasm::DisasmLine> lines, int rows, int scrollIdx)
    {
        if (lines.empty())
        {
            UI::Text({1, 0.5f, 0.5f, 1}, "无法反汇编 (无效地址或非代码段)");
            return;
        }
        if (scrollIdx >= static_cast<int>(lines.size()))
            scrollIdx = 0;
        auto visible = lines.subspan(static_cast<size_t>(scrollIdx));
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {S(4), S(4)});
        if (ImGui::BeginTable("Disasm", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("地址", ImGuiTableColumnFlags_WidthFixed, S(110));
            ImGui::TableSetupColumn("字节码", ImGuiTableColumnFlags_WidthFixed, S(90));
            ImGui::TableSetupColumn("指令", ImGuiTableColumnFlags_WidthFixed, S(60));
            ImGui::TableSetupColumn("操作数", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("操作", ImGuiTableColumnFlags_WidthFixed, S(80));
            ImGui::TableHeadersRow();
            int displayCount = std::min(static_cast<int>(visible.size()), rows);
            for (int i = 0; i < displayCount; ++i)
            {
                const auto &line = visible[i];
                if (!line.valid)
                    continue;
                ImGui::TableNextRow();
                ImGui::PushID(reinterpret_cast<void *>(line.address));
                ImGui::TableSetColumnIndex(0);
                UI::Text(line.address == base ? ImVec4{0.4f, 1.0f, 0.4f, 1.0f} : ImVec4{0.5f, 0.85f, 0.9f, 1.0f}, "%llX", static_cast<unsigned long long>(line.address));
                ImGui::TableSetColumnIndex(1);
                char bytesStr[48] = {0};
                for (size_t j = 0; j < line.size && j < 8; ++j)
                {
                    char tmp[4];
                    std::snprintf(tmp, sizeof(tmp), "%02X ", line.bytes[j]);
                    std::strcat(bytesStr, tmp);
                }
                UI::Text({0.6f, 0.6f, 0.6f, 1.0f}, "%s", bytesStr);
                ImGui::TableSetColumnIndex(2);
                UI::Text(getMnemonicColor(line.mnemonic), "%s", line.mnemonic);
                ImGui::TableSetColumnIndex(3);
                UI::Text({0.9f, 0.9f, 0.7f, 1.0f}, "%s", line.op_str);
                ImGui::TableSetColumnIndex(4);
                float btnW = S(35), btnH = S(24);
                if (isJumpInstruction(line.mnemonic))
                {
                    if (uintptr_t tAddr = parseJumpTarget(line.op_str))
                    {
                        if (UI::Btn("跳", {btnW, btnH}, {0.3f, 0.25f, 0.5f, 1.0f}))
                            App().memViewer.open(tAddr);
                        ImGui::SameLine();
                    }
                }
                if (UI::Btn("存", {btnW, btnH}, {0.2f, 0.4f, 0.25f, 1.0f}))
                    App().scanner.add(line.address);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();
    }

    static ImVec4 getMnemonicColor(const char *m)
    {
        if (!m)
            return {1, 1, 1, 1};
        if (m[0] == 'b' || !std::strncmp(m, "cb", 2) || !std::strncmp(m, "tb", 2) || !std::strcmp(m, "ret"))
            return {0.8f, 0.5f, 1.0f, 1.0f};
        if (!std::strncmp(m, "ld", 2) || !std::strncmp(m, "st", 2))
            return {0.5f, 0.7f, 1.0f, 1.0f};
        if (!std::strncmp(m, "add", 3) || !std::strncmp(m, "sub", 3) || !std::strncmp(m, "mul", 3) || !std::strncmp(m, "div", 3))
            return {0.5f, 1.0f, 0.5f, 1.0f};
        if (!std::strncmp(m, "cmp", 3) || !std::strncmp(m, "tst", 3))
            return {1.0f, 1.0f, 0.5f, 1.0f};
        if (!std::strncmp(m, "mov", 3))
            return {0.5f, 1.0f, 1.0f, 1.0f};
        if (!std::strcmp(m, "nop"))
            return {0.5f, 0.5f, 0.5f, 1.0f};
        return {1.0f, 1.0f, 1.0f, 1.0f};
    }
    static bool isJumpInstruction(const char *m) { return m && (m[0] == 'B' || !std::strncmp(m, "CB", 2) || !std::strncmp(m, "TB", 2) || !std::strcmp(m, "BL") || !std::strcmp(m, "BLR")); }
    static uintptr_t parseJumpTarget(const char *op_str)
    {
        if (!op_str)
            return 0;
        const char *p = std::strstr(op_str, "#0X");
        if (p)
            return std::strtoull(p + 1, nullptr, 16);
        p = std::strstr(op_str, "0X");
        return p ? std::strtoull(p, nullptr, 16) : 0;
    }
};

// ============================================================================
// 主函数
// ============================================================================

int main()
{

    if (RenderVK::init())
    {
        if (!Touch_Init())
        {
            std::println(stderr, "[Error] 初始化触摸失败");
            return 1;
        }
    }
    else
    {
        std::println(stderr, "[Error] 初始化图形引擎失败");
        return 1;
    }

    MainUI ui;
    //  渲染循环
    while (Config::g_Running)
    {

        Touch_UpdateImGui(); // 更新imgui输入事件

        RenderVK::drawBegin();

        ui.draw();

        RenderVK::drawEnd();

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // 清理触摸
    Touch_Shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    RenderVK::shutdown();

    // 停所有线程
    Utils::GlobalPool.force_stop();

    return 0;
}
