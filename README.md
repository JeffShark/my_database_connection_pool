# mysql-connect-pool

一个用于**学习与面试演示**的线程安全数据库连接池实现(C++17,单文件)。
`MySql` 是模拟连接句柄,重点不在真实驱动,而在于完整呈现:**池化思想、多线程借还、阻塞排队、RAII 防泄漏** 这一套设计,并附带 6 个可自动验证的测试用例。

> 面试复习配套文档见 [docs/interview-prep.md](docs/interview-prep.md)

## 特性

- **Meyers 懒汉式单例**:`static` 局部变量,构造线程安全(C++11 magic static)
- **互斥锁 + 条件变量**:池空时 `cv.wait(lock, 谓词)` 阻塞排队,归还时 `notify_one` 唤醒——谓词式等待避免丢失唤醒/虚假唤醒
- **RAII 连接包装**:`RAIIConnection` 构造即借、析构即还;`unique_ptr` 所有权模型在编译期杜绝"同一连接被两个线程同时持有"和"重复归还"
- **健壮性**:重复 `init()` 幂等拦截、空指针归还防御、`getStats()` 加锁读取一致快照
- **自带测试**:6 个 case 覆盖基础借还、RAII、8 线程并发压力(重复发放检测)、超额订阅排队、忘还饥饿演示、无锁 vs 加锁竞态对照,退出码 0/1 可直接接入 CI
- **跨平台**:Linux/macOS 直接编译;Windows 控制台自动切 UTF-8 输出(退出恢复),中文不乱码

## 目录结构

```
mysql-connect-pool/
├── main.cpp                  # 连接池实现 + 全部测试用例(单文件)
├── CMakeLists.txt            # 可选:CMake 构建
├── docs/
│   └── interview-prep.md     # 数据库连接池面试复习文档
├── .github/workflows/ci.yml  # GitHub Actions:编译 + 跑全部测试
├── .gitignore
└── LICENSE
```

## 快速开始

### 方式一:直接 g++

Windows(MinGW):

```bash
g++ -std=c++17 -O2 main.cpp -o pool_demo.exe
./pool_demo.exe
```

Linux / macOS(需要 pthread):

```bash
g++ -std=c++17 -O2 -pthread main.cpp -o pool_demo
./pool_demo
```

### 方式二:CMake

```bash
cmake -S . -B build
cmake --build build
./build/pool_demo        # Windows 下为 .\build\pool_demo.exe
```

全 PASS 时退出码为 0,否则为 1(适合 CI 判断)。

## 测试用例一览

| Case | 验证点 | 关键断言 |
|---|---|---|
| case1 | 基础借还 | 重复 init 被拒;借空后 可用=0/在用=4;归还后复原 |
| case2 | RAII 语义 | 作用域退出自动归还;移动构造不造成二次归还 |
| case3 | 8 线程 × 3000 次并发借还压力 | 同一连接从未被并发持有(冲突=0);借出总数守恒;池终态复原 |
| case4 | 超额订阅(容量 4 vs 12 任务) | 总耗时 ≈ 3 波(阻塞排队而非超卖) |
| case5 | "忘还"饥饿演示 | 借走不还 → 等待线程被饿住;归还后被 `cv` 唤醒 |
| case6 | 竞态对照 | 无锁共享计数发生丢失更新(结果不确定,UB);加锁/原子精确一致 |

典型输出片段(case3):

```
  → 8 线程 × 3000 次借还，耗时 224 ms
    conn id=1 被借出 6024 次
    ...
  → 同时持有冲突次数=0 借出总数=24000/24000 池状态：可用=4 在用=0
```

## 设计要点速览

```cpp
// 借出:池空则阻塞,双计数在锁内修改
std::unique_ptr<MySql> getConnectionFromPool() {
    std::unique_lock<std::mutex> lock(mtx);
    cv.wait(lock, [this] { return available_connection > 0; });
    auto conn = std::move(connection_list.front());
    connection_list.pop_front();
    available_connection--; used_connection++;
    lock.unlock();
    return conn;
}
```

- 借出返回 `unique_ptr` → 连接所有权随对象转移,**借用期间该连接绝对独占**;
- `RAIIConnection` 析构自动归还 → 作用域结束连接必然回池,**忘还/异常路径也不泄漏**;
- 计数与容器永远在同一把锁内修改 → 不会出现 lost update;
- `cv.wait` 必须配谓词 → 免疫虚假唤醒,也不丢唤醒。

## 已知边界(演示代码的取舍,面试可展开聊)

1. 进程退出时要求"所有连接已归还、无线程仍在借还",否则静态单例析构与残留线程间是未定义行为;
2. 获取连接**没有超时版本**——池永久耗尽时请求永久阻塞,真实项目应提供 `wait_for` 超时 + 失败策略;
3. 没有空闲回收、健康检查、动态扩容、泄漏回收——扩展路线图见 [docs/interview-prep.md](docs/interview-prep.md) 第 6 节;
4. `MySql` 仅为句柄占位,接入真实驱动(MariaDB/MySQL Connector/C++ 等)时只需替换句柄内部实现。

## 从压缩包推送到 GitHub

```bash
# 解压 zip 后进入仓库目录(目录内已含全部文件,无 .git)
cd mysql-connect-pool

# 1. 按需修改 LICENSE 中的版权占位
# 2. 初始化并推送
git init -b main
git add .
git commit -m "init: thread-safe mysql connection pool demo with tests"
git remote add origin https://github.com/<你的用户名>/<仓库名>.git
git push -u origin main
```

## License

MIT(见 [LICENSE](LICENSE),发布前请替换版权占位)。
