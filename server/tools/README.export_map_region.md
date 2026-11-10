# 导出灌溉现场到测试服

`export_map_region.py` 只需 Python 3.8+ 和标准库，不需要编译，不调用服务器程序。
默认导出中心 `(16254,5047)`、两轴各 ±400 的正方形，包含边界：
`15854 ≤ x ≤ 16654`、`4647 ≤ y ≤ 5447`，共 801×801 个可能的坐标。

## 在生产服务器导出

把脚本传到生产机器，`--source-dir` 指向**服务器运行数据目录**，里面应有
`map.db`、其他地图数据库、`settings/` 和 `biomeRandSeed.txt`，不是源码目录。

```sh
python3 export_map_region.py \
  --source-dir /path/to/production-server-runtime \
  --output-dir /tmp/czjy-region \
  --center-x 16254 --center-y 5047 --radius 400 \
  --live
```

输出目录必须不存在，并且必须在生产数据目录之外。失败时清理本次创建的输出；
既有目录和生产文件不会被覆盖。默认输出目录权限是 0700，配置中的 seedPepper
和区域内的密码数据不会打印到终端。脚本按完整记录分块顺序扫描，不把生产库或
哈希索引加载进内存。仍需读遍源文件，耗时与数据库总大小有关，扫描时显示进度。
Linux 可以用 `nice -n 19 ionice -c 3 python3 ...` 降低读取任务的优先级。

`--live` 允许生产服继续写入：各文件只读导出开始时已完整落盘的记录范围，
不追赶后续追加，忽略当时尚未写完的尾记录；文件被截断或替换时仍然报错。
`manifest.json` 记录源文件大小、导出条数、残缺尾部和扫描期间是否发生变化。

**这不是事务一致性快照**：库之间、甚至同一个库的不同记录可能来自不同时间，
一次容器修改或水移动涉及的多条记录可能只捕获到一部分；正在覆盖写的值也没有
原子读取保证。运行中的服务端没有这类快照协议，文件里的数据也不包含完整的
内存衰变队列、移动队列或尚未刷新的写缓冲。若机器已经提供文件系统快照，
优先以它作为输入并去掉 `--live`；不需要把整份快照传到测试机。

## 输出内容

| 文件 | 内容 |
|---|---|
| `tmp.db` | 区域内 `map.db` 的所有 slot/subcontainer 记录，包括空值、容器及子容器 |
| `mapTime.db` | 同区域物品和容器的绝对衰变时间，原样保留 |
| `floor.db` / `floorTime.db` | 地板及其衰变时间 |
| `biome.db` / `lookTime.db` | 人工地形覆盖及最后查看时间 |
| `persistentMap.db` | 区域内密码等持久数据 |
| `meta.db` | 上述物品/地板引用的元数据，缺失引用会报错 |
| `biomeRandSeed.txt` | 自然地图种子，保留未写入 map.db 的自然地形生成条件 |
| `mapDummyRecall.txt` | 若源目录存在此文件，保留区域内的 dummy 恢复记录 |
| `settings/` | 出生位置参数、epochSeconds、自然地形参数，以及下面列出的测试服设置 |
| `manifest.json` | 导出范围、文件统计、限制和配置清单 |

主库 `tmp.db` 是当前 `lineardb3.cpp` 能直接读取的原生文件，部署时改名为
`map.db`。缺失的辅助库会生成合法空库并警告；这意味着相应状态未能复制，
请检查警告，不要把它当作完整现场。旧 LINEARDB/KISSDB 等格式不在支持范围内。

## 在测试服载入

1. 使用单独的测试服运行目录和与生产一致的**服务端代码、物品/转换数据**。
   运行中的生产 map.db 可能包含动态生成的 use/variable dummy ID，数据版本
   不一致会导致这些 ID 对应错误。脚本不导出玩家、账号、血统或运营服务配置。
2. 停止**测试服**后备份其旧地图。使用空白测试地图目录最省事；不要混入旧的
   `mapDummyRecall.txt`，它会在启动时把旧 dummy 状态覆盖到新地图；如果区域包中
   有这个文件，使用区域包提供的版本。
3. 将整个区域包传到测试机。在测试服运行目录中把 `tmp.db` 安装为 `map.db`，
   其他 `.db`、`biomeRandSeed.txt` 与可选 `mapDummyRecall.txt` 按原名安装。
   不要让服务器直接打开 `tmp.db`，
   服务器固定读取 `map.db`。
4. 把区域包的 `settings/*.ini` 覆盖到**测试服** settings 目录，保留测试服自身的
   端口、鉴权、日志等配置。缺失的源 seedPepper/seedSpawnBoundingBox/epochSeconds
   及地形配置文件不会凭空生成：测试服应删除对应旧配置，让服务器采用与生产
   相同的默认值；缺失的配置会列在 manifest 的 warnings 中。
   默认 seedPepper 是代码中的完整字符串 `default pepper`，写成 ini 会被分词，
   所以“配置不存在”和“手写默认字符串”并不等价。
5. 启动测试服，用出生码 `czjy` 登录，先检查日志中 `seed evaluated to (...)`
   和 `New player ... (...)` 的坐标，再开展灌溉复现。

出生坐标依赖 `seedPepper.ini`、`seedSpawnBoundingBox.ini` 和服务器的 C++ 随机数
实现，与 map.db 内容本身无关。保持生产和测试的编译运行环境一致；跨 libstdc++ /
libc++ 时 `uniform_int_distribution` 的算法可能不同，需以实际登录坐标为准。
单独设置 `forceEveLocationX/Y` 无法覆盖带 seed 的定位，因为代码随后又计算 seed。

导出的 `settings/` 包含这些**仅用于测试**的覆盖：

- `forceEveOnSeededSpawn=1`：带出生码时独立出生，避免被分配为别人的孩子。
- `skipLookTimeCleanup=1`、`skipRemovedObjectCleanup=1`：避免启动清理改变现场。
- `longTermNoLookCullEnabled=0`：关闭测试过程中的长期未查看清理。
- `barrierOn=0`：避免测试服默认边界妨碍访问导出的远端区域。
- `useTestMap=0`、空 `specialMapPlacements`：避免其他测试地图覆盖现场。
- `nextMetadataID`：设置为区域内最大的已引用元数据 ID，避免初次写入覆盖它。

定时器的绝对时间未重写，测试服启动时可能执行已经到期的转换；建议尽快载入。
活跃水量和玩家位置本来就会变化，区域快照是可重复调试的起点，不是运行中服务器
每个内存状态的复制品。每次重做测试都应恢复原始区域包。

## 本地验证

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s server/tools/tests -v
```

测试会编译实际 `lineardb3.cpp`：先由原生 DB API 创建源库，再用同一 API 打开
导出的库，逐键核对值与记录数。还覆盖边界、空值、容器/子容器、元数据关联、
只读源文件、并发追加/覆盖、残缺尾部、截断、替换与输出目录保护。测试需要
clang++ 或 g++，运行导出脚本本身不需要。
