#include <iostream>
#include <string>
#include <list>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <thread>
#include <atomic>
#include <vector>
#include <chrono>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <algorithm>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// =============================================================
// 数据库连接池 Demo（MySql 只是模拟句柄，重在演示"池化 + 借还 + RAII"思想）
// 编译: g++ -std=c++17 -O2 main.cpp -o pool_demo.exe
// =============================================================

// ---------- 全局小工具 ----------

// Windows 控制台编码适配：
// 本文件以 UTF-8 保存，中文字符串经 std::cout 输出的是 UTF-8 字节；
// 而 Windows 控制台默认按本地代码页（中文系统 = GBK/936）解释字节流，
// 直接输出就会乱码（如"连接"变"杩炴帴"）。
// 这里在程序运行期间把控制台输出代码页临时切到 UTF-8(65001)，退出时恢复，
// 不影响用户终端后续使用；输出被重定向到文件/管道时（GetConsoleOutputCP 返回 0）自动跳过。
#ifdef _WIN32
class ConsoleUtf8Guard {
public:
    ConsoleUtf8Guard() : oldCp_(GetConsoleOutputCP()), changed_(false) {
        if (oldCp_ != 0 && oldCp_ != CP_UTF8) {
            changed_ = (SetConsoleOutputCP(CP_UTF8) != 0);
        }
    }
    ~ConsoleUtf8Guard() {
        if (changed_) {
            SetConsoleOutputCP(oldCp_);  // 恢复用户控制台原有的代码页
        }
    }

private:
    UINT oldCp_;
    bool changed_;
};
#else
class ConsoleUtf8Guard {
public:
    ConsoleUtf8Guard() {}
};
#endif

// 给每个模拟连接发一个全局唯一 id，方便观察池的复用情况
std::atomic<int> g_mysqlIdGen{0};

// 跨线程打印加锁，避免输出交错（仅测试输出用）
std::mutex g_printMtx;
template <typename... Args>
void tprint(Args&&... args) {
    std::lock_guard<std::mutex> lock(g_printMtx);
    int dummy[] = {0, ((std::cout << std::forward<Args>(args)), 0)...};
    (void)dummy;
    std::cout << std::endl;
}

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// 忙等微秒级时间（Windows 上 sleep_for 精度粗糙，微秒停顿用自旋更可控）
void spinUs(long long us) {
    auto end = std::chrono::steady_clock::now() + std::chrono::microseconds(us);
    while (std::chrono::steady_clock::now() < end) {
    }
}

// ---------- 模拟的数据库连接句柄 ----------

class MySql {
public:
    MySql(std::string host, std::string user, std::string password, std::string database, std::string port)
        : host(host), user(user), password(password), database(database), port(port) {
        id_ = ++g_mysqlIdGen;  // 每个连接对象有唯一 id
    }

    int id() const { return id_; }

    void show_connect() {
        std::cout << "Connecting to MySQL database at " << host << " with user " << user
                  << " (conn id=" << id_ << ")" << std::endl;
    }

private:
    std::string host;
    std::string user;
    std::string password;
    std::string database;
    std::string port;
    int id_;
};

// ---------- 线程安全的连接池 ----------

struct PoolStats {
    int available_connection;  // 池中空闲连接数
    int used_connection;       // 已被借出的连接数
};

class MySqlPool {
public:
    static MySqlPool* GetInstance() {
        static MySqlPool instance;  // Meyers 单例，C++11 起线程安全的局部静态初始化
        return &instance;
    }

    // 单例需要禁用拷贝/赋值（加 const 的版本更能拦截右值）
    MySqlPool(const MySqlPool&) = delete;
    MySqlPool& operator=(const MySqlPool&) = delete;

    // 初始化连接池。加锁 + 幂等保护：重复调用会被拒绝（原代码可重复调用，会破坏计数）
    bool init(std::string host, std::string user, std::string password, std::string database,
              std::string port, int max_connection) {
        std::lock_guard<std::mutex> lock(mtx);
        if (initialized_) {
            return false;
        }
        for (int i = 0; i < max_connection; i++) {
            connection_list.push_back(std::make_unique<MySql>(host, user, password, database, port));
        }
        available_connection = max_connection;
        used_connection = 0;
        initialized_ = true;
        return true;
    }

    // 加锁读取两个计数，保证读到的快照一致（原代码无此接口，测试也没法做断言）
    PoolStats getStats() {
        std::lock_guard<std::mutex> lock(mtx);
        PoolStats s;
        s.available_connection = available_connection;
        s.used_connection = used_connection;
        return s;
    }

    // 借出连接：池空时阻塞等待。
    // 注意：必须在 init() 之后调用，否则谓词永不满足会永久阻塞；
    // 真实项目建议提供带超时（wait_for）的版本 + 健康检查。
    std::unique_ptr<MySql> getConnectionFromPool() {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [this] { return available_connection > 0; });  // 谓词式等待，无丢失唤醒

        std::unique_ptr<MySql> connection = std::move(connection_list.front());
        connection_list.pop_front();
        available_connection--;
        used_connection++;

        lock.unlock();
        return connection;
    }

    // 归还连接：先解锁再 notify，避免唤醒的线程立刻又阻塞在锁上
    void returnConnectionToPool(std::unique_ptr<MySql> connection) {
        if (!connection) {  // 防御：空指针不允许进池（否则后面 front() 会借出空连接）
            return;
        }
        std::unique_lock<std::mutex> lock(mtx);
        connection_list.push_back(std::move(connection));
        available_connection++;
        used_connection--;
        lock.unlock();
        cv.notify_one();
    }

    // 析构要求：进程退出时所有连接已归还、且无线程仍在借还。
    // 若某线程"忘还"连接后进程退出，静态单例析构与残留线程间是未定义行为。
    ~MySqlPool() {
        destroyConnections();
    }

    void destroyConnections() {
        for (auto& connection : connection_list) {
            connection.reset();
        }
        connection_list.clear();
    }

private:
    MySqlPool() {
        available_connection = 0;
        used_connection = 0;
        initialized_ = false;
    }

    int available_connection;
    int used_connection;
    bool initialized_;
    std::condition_variable cv;
    std::mutex mtx;
    std::list<std::unique_ptr<MySql>> connection_list;
};

// ---------- RAII 封装：构造即借、析构即还 ----------

class RAIIConnection {
public:
    RAIIConnection() {
        MySqlPool* pool = MySqlPool::GetInstance();
        m_mysql = pool->getConnectionFromPool();  // 池空时在此阻塞
    }

    // 显式移动构造：被 move 走的旧对象 m_mysql 为空，析构不会二次归还
    RAIIConnection(RAIIConnection&& other) noexcept : m_mysql(std::move(other.m_mysql)) {}
    RAIIConnection(const RAIIConnection&) = delete;
    RAIIConnection& operator=(const RAIIConnection&) = delete;

    ~RAIIConnection() {
        if (m_mysql) {
            MySqlPool::GetInstance()->returnConnectionToPool(std::move(m_mysql));
        }
    }

    MySql* operator->() {
        return m_mysql.get();
    }

private:
    std::unique_ptr<MySql> m_mysql;  // unique_ptr 保证同一连接只有唯一持有者，不会重复归还
};

// =============================================================
// 测试用例（每个 case 结束后都必须把池恢复成 可用=容量 / 在用=0）
// =============================================================

bool test1_basicGetReturn() {
    MySqlPool* pool = MySqlPool::GetInstance();
    bool ok = true;

    // ① 重复 init 应被拒绝（原代码会在这里把池搞坏）
    bool dup = pool->init("127.0.0.1", "root", "123456", "demo", "3306", 8);
    tprint("  → 重复调用 init(容量8)：", dup ? "被接受(错误!)" : "被拒绝(正确)");
    ok = ok && !dup;

    // ② 单线程借空全部 4 条连接
    tprint("  → 单线程借空全部连接：");
    std::vector<std::unique_ptr<MySql>> conns;
    for (int i = 0; i < 4; i++) {
        conns.push_back(pool->getConnectionFromPool());
        tprint("    拿到 conn id=", conns.back()->id());
    }
    PoolStats s = pool->getStats();
    tprint("  → 借空后：可用=", s.available_connection, " 在用=", s.used_connection);
    ok = ok && (s.available_connection == 0 && s.used_connection == 4);

    // ③ 手动归还后计数复原
    for (auto& c : conns) {
        pool->returnConnectionToPool(std::move(c));
    }
    s = pool->getStats();
    tprint("  → 全部归还后：可用=", s.available_connection, " 在用=", s.used_connection);
    ok = ok && (s.available_connection == 4 && s.used_connection == 0);
    return ok;
}

bool test2_raiiAutoReturn() {
    MySqlPool* pool = MySqlPool::GetInstance();
    bool ok = true;
    {
        RAIIConnection a;
        RAIIConnection b;
        a->show_connect();  // 通过 -> 使用连接
        PoolStats s = pool->getStats();
        tprint("  → 作用域内持有 2 个 RAII 连接：可用=", s.available_connection, " 在用=", s.used_connection);
        ok = ok && (s.available_connection == 2 && s.used_connection == 2);
    }  // 离开作用域自动归还
    PoolStats s = pool->getStats();
    tprint("  → 离开作用域后自动归还：可用=", s.available_connection, " 在用=", s.used_connection);
    ok = ok && (s.available_connection == 4 && s.used_connection == 0);

    // 移动语义：被 move 的 RAII 对象析构时不会二次归还
    {
        RAIIConnection a;
        RAIIConnection b = std::move(a);  // a 的连接转移到 b（仅 1 条在外）
        PoolStats s2 = pool->getStats();
        ok = ok && (s2.available_connection == 3 && s2.used_connection == 1);
        tprint("  → RAIIConnection 移动后仍只借出 1 条：可用=", s2.available_connection);
    }
    s = pool->getStats();
    ok = ok && (s.available_connection == 4 && s.used_connection == 0);
    tprint("  → 移动后的对象析构未造成二次归还：可用=", s.available_connection);
    return ok;
}

// 多线程压力测试：验证不存在竞态——同一连接绝不会同时被两个线程持有，
// 借出/归还总数严格一致，最终计数复原。
bool test3_multiThreadStress() {
    MySqlPool* pool = MySqlPool::GetInstance();
    const int kThreads = 8;
    const int kIters = 3000;

    std::mutex setMtx;                  // 仅保护下面的校验集合（与连接池本身无关）
    std::unordered_set<MySql*> inUse;   // 当前正被某线程持有的原始指针
    int duplicateCount = 0;             // 若同一时刻同一连接被借给两个线程 => 竞态命中
    std::vector<std::unordered_map<int, int>> usage(kThreads);  // 每个线程拿到各 conn id 的次数

    auto t0 = nowMs();
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; t++) {
        ts.emplace_back([&, t] {
            auto& local = usage[t];
            for (int i = 0; i < kIters; i++) {
                RAIIConnection rc;            // 借连接（池空则阻塞等待）
                MySql* raw = rc.operator->(); // 持有期内该连接由本线程独占
                {
                    std::lock_guard<std::mutex> lock(setMtx);
                    if (!inUse.insert(raw).second) {
                        duplicateCount++;  // 撞见"同一连接被并发持有"= 竞态
                    }
                }
                local[raw->id()]++;
                spinUs(30);  // 模拟一条 SQL（微秒级，制造持有窗口）
                {
                    std::lock_guard<std::mutex> lock(setMtx);
                    inUse.erase(raw);  // 释放前从"在用集合"移除
                }
            }  // RAII：自动归还
        });
    }
    for (auto& th : ts) {
        th.join();
    }
    long long elapsed = nowMs() - t0;

    // 汇总每个连接 id 被借出总次数
    std::map<int, long long> totalUsage;
    for (auto& local : usage) {
        for (auto& kv : local) {
            totalUsage[kv.first] += kv.second;
        }
    }
    tprint("  → ", kThreads, " 线程 × ", kIters, " 次借还，耗时 ", elapsed, " ms");
    for (auto& kv : totalUsage) {
        tprint("    conn id=", kv.first, " 被借出 ", kv.second, " 次");
    }

    long long expected = (long long)kThreads * kIters;
    bool ok = true;
    ok = ok && (duplicateCount == 0);
    long long sum = 0;
    for (auto& kv : totalUsage) {
        sum += kv.second;
    }
    ok = ok && (sum == expected);  // 没有任何一次借出/归还丢失
    PoolStats s = pool->getStats();
    ok = ok && (s.available_connection == 4 && s.used_connection == 0);
    tprint("  → 同时持有冲突次数=", duplicateCount, " 借出总数=", sum, "/", expected,
           " 池状态：可用=", s.available_connection, " 在用=", s.used_connection);
    return ok;
}

// 超额订阅（容量 4、任务 12）：验证池空时线程会阻塞排队，
// 而不是"超卖"——若连接被重复发放，总耗时会接近一波 150ms，本断言会失败。
bool test4_oversubscribeQueue() {
    MySqlPool* pool = MySqlPool::GetInstance();
    const int kTasks = 12;
    const int kHoldMs = 150;
    bool ok = true;

    std::vector<long long> waitMs(kTasks, 0);
    auto t0 = nowMs();
    std::vector<std::thread> ts;
    for (int t = 0; t < kTasks; t++) {
        ts.emplace_back([&, t] {
            auto st = nowMs();
            std::unique_ptr<MySql> c = pool->getConnectionFromPool();  // 池空时在此阻塞排队
            waitMs[t] = nowMs() - st;                                  // 排队等待时长
            std::this_thread::sleep_for(std::chrono::milliseconds(kHoldMs));  // 模拟查询
            pool->returnConnectionToPool(std::move(c));
        });
    }
    for (auto& th : ts) {
        th.join();
    }
    long long makespan = nowMs() - t0;

    std::vector<long long> w(waitMs.begin(), waitMs.end());
    std::sort(w.begin(), w.end());
    tprint("  → 容量 4、任务 12（每任务持连接 150ms），总耗时 ", makespan, " ms（理想 ≈ 3 波 ≈ 450ms）");
    tprint("  → 各任务排队等待时长(ms)：min=", w.front(), " median=", w[w.size() / 2],
           " max=", w.back());

    ok = ok && (makespan >= 400);  // 若连接被"超卖/重复发放"，≈150ms 即可跑完，这里会 FAIL
    ok = ok && (w.back() >= 140);  // 存在真实排队等待
    return ok;
}

// "忘还"演示：借出后不归还 => 池耗尽 => 后续线程永久饥饿（真实场景直接卡死）。
// 这里做安全版演示：饿一个等待线程 300ms 后手动归还一条，验证它能被唤醒。
bool test5_forgetReturnStarvation() {
    MySqlPool* pool = MySqlPool::GetInstance();
    bool ok = true;

    // 模拟有人忘记归还：把 4 条连接全部借走并"扣下"
    std::vector<std::unique_ptr<MySql>> forgot;
    for (int i = 0; i < 4; i++) {
        forgot.push_back(pool->getConnectionFromPool());
    }
    PoolStats s = pool->getStats();
    tprint("  → 模拟 4 条连接全部被借走且“忘记归还”：可用=", s.available_connection,
           " 在用=", s.used_connection, "（此后新请求将永远阻塞）");

    std::atomic<bool> waiterDone{false};
    std::thread waiter([&] {
        RAIIConnection rc;  // 池空，会一直阻塞在 getConnectionFromPool
        waiterDone = true;
    });  // 离开作用域自动归还

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    bool starved = !waiterDone.load();
    tprint("  → 300ms 后等待线程仍未拿到连接：", starved ? "是（已被饿住，符合预期）" : "否（异常！）");
    ok = ok && starved;

    // 修复：归还 1 条，等待线程应被 cv 唤醒
    pool->returnConnectionToPool(std::move(forgot[0]));
    waiter.join();
    tprint("  → 归还 1 条后，等待线程被唤醒并完成：", waiterDone.load() ? "是" : "否（异常！）");
    ok = ok && waiterDone.load();

    // 清理：归还剩余 3 条，池恢复原状
    for (int i = 1; i < 4; i++) {
        pool->returnConnectionToPool(std::move(forgot[i]));
    }
    s = pool->getStats();
    ok = ok && (s.available_connection == 4 && s.used_connection == 0);
    tprint("  → 全部归还后：可用=", s.available_connection, " 在用=", s.used_connection);
    return ok;
}

// 竞态现象对照演示：同一段共享计数代码——
// (1) 不加锁：发生丢失更新（lost update），结果小于期望值（严格说是 UB，仅用于演示）；
// (2) 加锁 / 原子变量：结果精确。
// 连接池内部的 available/used 计数正是靠 mtx 保护才不会有这种问题（见 case3）。
bool test6_raceDemonstration() {
    const int kThreads = 8;
    const int kEach = 200000;
    unsigned long long expected = (unsigned long long)kThreads * kEach;

    // (1) 无任何同步 —— 经典竞态
    volatile unsigned long long cnt1 = 0;
    {
        std::vector<std::thread> ts;
        for (int t = 0; t < kThreads; t++) {
            ts.emplace_back([&] {
                for (int i = 0; i < kEach; i++) {
                    cnt1++;
                }
            });
        }
        for (auto& th : ts) {
            th.join();
        }
    }
    unsigned long long lost = expected - (unsigned long long)cnt1;  // 只有丢失不会超
    tprint("  (1) 无锁共享计数：结果=", (unsigned long long)cnt1, " 期望=", expected,
           lost > 0 ? ("  ← 丢失 " + std::to_string(lost) + " 次更新（竞态！）").c_str()
                    : "  （本次未丢，但数据竞争本身仍是 UB）");

    // (2) 互斥锁保护 —— 正确
    unsigned long long cnt2 = 0;
    std::mutex m;
    {
        std::vector<std::thread> ts;
        for (int t = 0; t < kThreads; t++) {
            ts.emplace_back([&] {
                for (int i = 0; i < kEach; i++) {
                    std::lock_guard<std::mutex> lock(m);
                    cnt2++;
                }
            });
        }
        for (auto& th : ts) {
            th.join();
        }
    }
    tprint("  (2) 互斥锁保护：结果=", cnt2, " 期望=", expected,
           cnt2 == expected ? "  ✓ 精确一致" : "  ✗ 不一致(异常)");

    // (3) 原子变量 —— 正确
    std::atomic<unsigned long long> cnt3{0};
    {
        std::vector<std::thread> ts;
        for (int t = 0; t < kThreads; t++) {
            ts.emplace_back([&] {
                for (int i = 0; i < kEach; i++) {
                    cnt3++;
                }
            });
        }
        for (auto& th : ts) {
            th.join();
        }
    }
    tprint("  (3) 原子变量：结果=", cnt3.load(), " 期望=", expected,
           cnt3.load() == expected ? "  ✓ 精确一致" : "  ✗ 不一致(异常)");
    return true;  // 演示用例：不断言，竞态现象展示在输出里
}

// ---------- 测试驱动 ----------

template <typename Fn>
bool runCase(const std::string& name, Fn&& fn) {
    auto t0 = std::chrono::steady_clock::now();
    std::cout << "----------------------------------------\n[运行] " << name << std::endl;
    bool ok = fn();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0)
                  .count();
    std::cout << "[结果] " << (ok ? "PASS" : "FAIL") << "  （本 case 耗时 " << ms << " ms）\n"
              << std::endl;
    return ok;
}

int main() {
    ConsoleUtf8Guard consoleGuard;  // Windows 下把控制台输出切到 UTF-8，退出时自动恢复
    std::cout << "==== 连接池多线程测试 ====  硬件线程数: " << std::thread::hardware_concurrency()
              << std::endl;

    // 全局单例只 init 一次，池容量 4
    MySqlPool* pool = MySqlPool::GetInstance();
    bool first = pool->init("127.0.0.1", "root", "123456", "demo", "3306", 4);
    std::cout << "init 首次调用: " << (first ? "成功（预创建 4 条连接）" : "失败") << std::endl;

    int pass = 0, fail = 0;
    bool r;
    r = runCase("case1 基础借还：重复 init 拦截 + 手动借空/归还 + 计数一致", test1_basicGetReturn);
    r ? pass++ : fail++;
    r = runCase("case2 RAII：作用域自动归还 + 移动语义不二次归还", test2_raiiAutoReturn);
    r ? pass++ : fail++;
    r = runCase("case3 多线程压力：8 线程×3000 次并发借还（无重复发放/无计数丢失）",
                test3_multiThreadStress);
    r ? pass++ : fail++;
    r = runCase("case4 超额订阅：容量 4 对 12 任务，验证阻塞排队而非超卖", test4_oversubscribeQueue);
    r ? pass++ : fail++;
    r = runCase("case5 “忘还”演示：池耗尽导致线程饥饿，归还后被唤醒", test5_forgetReturnStarvation);
    r ? pass++ : fail++;
    r = runCase("case6 竞态对照：无锁计数丢失更新 vs 加锁/原子精确一致", test6_raceDemonstration);
    r ? pass++ : fail++;

    PoolStats final = pool->getStats();
    std::cout << "==== 测试结束: PASS " << pass << " / FAIL " << fail
              << "   池终态: 可用=" << final.available_connection
              << " 在用=" << final.used_connection << " ====" << std::endl;
    return fail == 0 ? 0 : 1;
}
