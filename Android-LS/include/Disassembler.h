#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include "../capstone/include/capstone/capstone.h"

namespace Disasm
{

    struct DisasmLine
    {
        bool valid = false;
        uint64_t address = 0;
        size_t size = 0;
        uint8_t bytes[16] = {0};
        char mnemonic[32] = {0};
        char op_str[160] = {0};
    };

    class Disassembler
    {
    public:
        Disassembler() : m_handle(0), m_valid(false), m_arch(CS_ARCH_MAX)
        {
            // 打印 Capstone 版本信息
            int major = 0, minor = 0;
            cs_version(&major, &minor);
            printf("[*] Capstone 版本: %d.%d\n", major, minor);

            // 列出支持的架构
            PrintSupportedArchs();

            // 尝试初始化 ARM64 (使用 CS_ARCH_AARCH64)
            if (TryInit(CS_ARCH_AARCH64, CS_MODE_LITTLE_ENDIAN, "ARM64/AArch64"))
            {
                return;
            }

            // 尝试 ARM32 作为备选
            if (TryInit(CS_ARCH_ARM, CS_MODE_ARM, "ARM32"))
            {
                printf("[!] 警告: 使用 ARM32 模式，64位指令可能无法正确反汇编\n");
                return;
            }

            printf("[-] 所有架构初始化均失败！\n");
        }

        ~Disassembler()
        {
            if (m_valid && m_handle)
            {
                cs_close(&m_handle);
            }
        }

        Disassembler(const Disassembler &) = delete;
        Disassembler &operator=(const Disassembler &) = delete;

        bool IsValid() const { return m_valid; }

        const char *GetLastError() const
        {
            if (!m_valid)
                return "反汇编器未初始化";
            return cs_strerror(cs_errno(m_handle));
        }

        std::vector<DisasmLine> Disassemble(uint64_t address, const uint8_t *buffer,
                                            size_t size, size_t maxCount = 0)
        {
            std::vector<DisasmLine> results;

            if (!m_valid)
            {
                printf("[-] Debug: 反汇编器未初始化\n");
                return results;
            }

            //  打印入参调试信息
            printf("[*] Debug: 尝试反汇编 Address: %llX, Size: %zu\n", address, size);

            //  检查数据是否全为 0 (常见错误：读取失败导致 buffer 全 0)
            bool allZero = true;
            for (size_t i = 0; i < (size < 16 ? size : 16); i++)
            {
                if (buffer[i] != 0)
                {
                    allZero = false;
                    break;
                }
            }

            if (allZero)
            {
                printf("[-] Debug: 警告！传递给反汇编器的数据全为 0 (前16字节)。\n"
                       "可能原因: 驱动读取内存失败，或该地址确实为空。\n");
                // 这里不 return，尝试让 Capstone 处理（ARM64 全0 是 udf 指令）
            }

            // 对齐处理
            if (m_arch == CS_ARCH_AARCH64 && (address & 0x3))
            {
                uint64_t offset = address & 0x3;
                address &= ~(uint64_t)0x3;
                if (offset < size)
                {
                    buffer += offset;
                    size -= offset;
                }
            }

            cs_insn *insn = nullptr;
            //  执行反汇编
            size_t count = cs_disasm(m_handle, buffer, size, address, maxCount, &insn);

            if (count == 0)
            {
                cs_err err = cs_errno(m_handle);
                printf("[-] Debug: 反汇编结果为 0 条。\n");
                printf("    Capstone 错误码: %d (%s)\n", err, cs_strerror(err));
                printf("    原始数据(前8字节): ");
                for (size_t i = 0; i < 8 && i < size; i++)
                    printf("%02X ", buffer[i]);
                printf("\n");
                return results;
            }

            //  成功，转换数据
            results.reserve(count);
            for (size_t i = 0; i < count; i++)
            {
                DisasmLine line;
                line.valid = true;
                line.address = insn[i].address;
                line.size = insn[i].size;

                size_t copyLen = (insn[i].size < sizeof(line.bytes)) ? insn[i].size : sizeof(line.bytes);
                memcpy(line.bytes, insn[i].bytes, copyLen);
                strncpy(line.mnemonic, insn[i].mnemonic, sizeof(line.mnemonic) - 1);
                strncpy(line.op_str, insn[i].op_str, sizeof(line.op_str) - 1);

                // 转换为大写
                for (char *p = line.mnemonic; *p; ++p)
                    *p = toupper((unsigned char)*p);
                for (char *p = line.op_str; *p; ++p)
                    *p = toupper((unsigned char)*p);

                results.push_back(line);
            }

            cs_free(insn, count);
            return results;
        }

    private:
        csh m_handle;
        bool m_valid;
        cs_arch m_arch;

        bool TryInit(cs_arch arch, cs_mode mode, const char *name)
        {
            // 先检查架构是否支持
            if (!cs_support(arch))
            {
                printf("[-] 架构 %s (%d) 未编译支持\n", name, arch);
                return false;
            }

            cs_err err = cs_open(arch, mode, &m_handle);
            if (err != CS_ERR_OK)
            {
                printf("[-] %s 初始化失败: %s\n", name, cs_strerror(err));
                return false;
            }

            cs_option(m_handle, CS_OPT_DETAIL, CS_OPT_ON);
            m_arch = arch;
            m_valid = true;
            printf("[+] 反汇编器初始化成功: %s\n", name);
            return true;
        }

        void PrintSupportedArchs()
        {
            printf("[*] 支持的架构:\n");

            // CS_ARCH_ARM = 0, CS_ARCH_AARCH64 = 1
            struct
            {
                cs_arch arch;
                const char *name;
            } archs[] = {
                {CS_ARCH_ARM, "ARM32"},
                {CS_ARCH_AARCH64, "ARM64/AArch64"},
                {CS_ARCH_X86, "X86"},
                {CS_ARCH_MIPS, "MIPS"},
            };

            for (auto &a : archs)
            {
                bool supported = cs_support(a.arch);
                printf("    %s: %s\n", a.name, supported ? "[OK]" : "[X]");
            }
        }
    };

} // namespace Disasm