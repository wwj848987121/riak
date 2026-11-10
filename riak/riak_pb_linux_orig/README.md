# Riak PB 文件扩展

为本项目提供 PHP 8.4 原生 Riak 文件存取能力，通过明文 TCP 使用 Protocol Buffers 协议连接 Riak 2.0.0。当前实现面向 Linux，支持上传、读取、删除、连通性检查、服务端信息和 `allow_mult` 属性设置。

## 构建目标

| 项目 | 目标环境 |
| --- | --- |
| PHP | 8.4.27，NTS，非 Debug |
| 系统与架构 | Linux x86_64 |
| PHP Extension API | `20240924` |
| PHP Extension Build | `API20240924,NTS` |
| 线上 PHP 运行方式 | Apache 2 Handler |
| 线上 PHP 编译信息 | RHEL 10.2，GCC 14.3.1 |
| Riak | 2.0.0，明文 TCP，无用户名密码、无 TLS |
| PB 端口 | **8129，用户已确认** |

PHP 客户端 `RiakClient` 不写死地址，生产地址写在各应用的 `config/riak_config.php`（不在文档中记录），其他环境用 `RIAK_PB_*` 环境变量覆写（见下文“PHP 应用接入”）。旧项目代码中的 `8087` 仅是历史端口，不再使用。

源码当前只接受 PHP 8.4，使用 POSIX 网络接口；不包含 Windows DLL 构建配置。PHP 页面中的 RHEL 10.2 是 PHP 的编译系统信息，实际部署容器的发行版和基础镜像应另行核对。

## 开发与本地验证

从仓库根目录运行：

```sh
docker build -t cdm-riak-pb:php84 extensions/riak_pb
docker run --rm cdm-riak-pb:php84
```

Dockerfile 使用 `php:8.4.27-cli-bookworm`，编译扩展后执行 protobuf 编解码测试和本地模拟 Riak 的 TCP 测试。测试覆盖二进制内容、分段收包、超时、错误响应、非法帧、未知 protobuf 字段和 siblings 冲突。模拟服务只监听容器本地，不访问真实 Riak。

构建产物在容器内：

```text
/build/riak_pb/modules/riak_pb.so
```

该镜像用于独立构建和协议测试。它不代表 Debian 构建产物已在生产 RHEL 环境验证，也不代表已完成真实 Riak 2.0.0 联调。

本地已在 PHP 8.4.27 NTS 中通过严格编译（`-Wall -Wextra -Werror`）、编解码器
AddressSanitizer/UndefinedBehaviorSanitizer 检查及完整模拟 TCP 测试。
导出的测试构建位于本目录 `modules/riak_pb.so`（构建产物不纳入 Git），仅依赖系统 libc。

### 测试环境兼容性验证（2026-11-09）

已将上述现成 `.so` 临时复制到测试环境 `<test-host>` 的 CDM PHP 8.4 容器进行验证，未重新编译：

- 容器：`<namespace>/<pod>`，容器名 `apache`。
- 镜像：`<registry>/bom/httpd-php:84v4`。
- 镜像摘要：`sha256:341074d42dba789b45bd192c288664a93a383467724a782c64a08fd18a69bc49`。
- 系统：AlmaLinux 10.2、x86_64、glibc 2.39。
- PHP：8.4.27（Remi）、NTS、非 Debug、Extension API `20240924`。
- 扩展 SHA-256：`2f1a49209db683c97beaf4ecaa16f88e309e53de160c5348c34cfb7e93f3bf74`。
- 动态库依赖和 CLI 加载通过；分别验证了 `php -n` 及原有 PHP 配置下加载扩展。
- 本地模拟 Riak TCP 测试 **61 项通过**，涵盖二进制上传、读取、删除、超时和异常帧。
- 独立临时 Apache 2.4.63 仅监听容器 `127.0.0.1` 随机端口，沿用 `/etc/php.ini` 和
  `/etc/php.d`，额外扫描临时扩展 ini。Web 返回 `sapi=apache2handler`、
  `riak_pb_loaded=true`，全部 6 个函数存在，原生参数校验通过。

测试未修改业务 PHP 配置、未重启业务 Apache、未访问真实 Riak。临时 Apache 已停止，
容器和宿主机的临时文件已清理；业务容器保持 Ready，重启次数为 0。
这是该测试镜像的兼容性结果，不代表扩展已永久安装，也不代表实际 Riak `8129` 读写已联调完成。

主要源码：

- `riak_pb.c`：PHP 函数、参数校验和 Riak 请求/响应处理。
- `pb_codec.c`、`pb_codec.h`：最小 protobuf wire 编解码，无第三方 protobuf 库依赖。
- `riak_transport.c`、`riak_transport.h`：TCP 连接、帧边界、分段读写和超时。
- `tests/codec_test.c`、`tests/mock_server.py`、`tests/integration.php`：本地测试。

## 生产构建与加载

在与线上相同的基础镜像和 CPU 架构中，准备匹配线上 PHP 的开发头文件、`phpize`、`php-config`、C 编译器、make 和 autoconf。先确认工具对应 PHP 8.4.27，`phpize --version` 中的 PHP API 为 `20240924`：

```sh
php -v
phpize --version
php-config --version

cd extensions/riak_pb
phpize
./configure --enable-riak-pb --with-php-config="$(command -v php-config)"
make -j2
make install
```

`make install` 需要目标扩展目录的写权限。线上页面显示扩展目录为 `/usr/lib64/php/modules`，实际以目标 PHP 的 `extension_dir` 为准。

在该 PHP 实际加载的配置目录中新增扩展配置，例如 `/etc/php.d/40-riak_pb.ini`：

```ini
extension=riak_pb.so
```

重新启动 Apache/PHP 进程后检查：

```sh
php --ini
php --ri riak_pb
```

CLI 和 Apache 可以使用不同的 PHP 配置；Web 侧也应确认 `extension_loaded('riak_pb')` 为 `true`。

## PHP 应用接入

项目只有一个 Riak 客户端 `Wriak\File\RiakClient`（`cdm_domains/wriak/File/RiakClient.php`），只走 PB，
没有 HTTP 实现，也不会回退 HTTP。缺少扩展时构造即报错，PB 请求失败时抛出异常。

连接配置按以下优先级读取：

1. 构造参数 `pb_host`、`pb_port`、`pb_timeout_ms`；
2. 环境变量 `RIAK_PB_HOST`、`RIAK_PB_PORT`、`RIAK_PB_TIMEOUT_MS`；
3. 各应用 `config/riak_config.php` 合并到的 `\DomainFlight\Config::$riak_config`
   （CI 应用由 `load_flight_domain()` 合并，Fuel 由 `fuel/app/bootstrap.php` 合并）。

代码中不写死地址，三处都没有值时报错。环境变量必须配置到实际的 Apache/PHP 进程中；只在交互式终端中 `export` 不会修改已运行 Web 进程的环境。主机值可以是 DNS 名称、IPv4 或不带方括号的 IPv6，不能包含协议前缀或端口。端口范围为 `1..65535`，超时范围为 `1..600000` 毫秒。

直接使用客户端：

```php
$client = new \Wriak\File\RiakClient();            // 使用配置
$client->put('cdm', 'file.txt', $content, 'text/plain');
$object = $client->get('cdm', 'file.txt');          // ['content' => ..., 'content_type' => ...] 或 null
```

通过 `RiakFileServer` 指定 PB 主机和端口：

```php
$server = \Wriak\File\RiakFileServer::getInstance('<RIAK_PB_HOST>', 8129);
// 或使用构造函数：
$server = new \Wriak\File\RiakFileServer('<RIAK_PB_HOST>', 8129);
$result = $server->move('/path/to/file.txt', 'file.txt', 'cdm');
```

主机只填 IP 或域名，不带 `tcp://` 或 `http://` 前缀；省略端口时按上面的优先级读取。需要指定超时时使用配置数组：

```php
$server = new \Wriak\File\RiakFileServer([
    'pb_host' => '<RIAK_PB_HOST>',
    'pb_port' => 8129,
    'pb_timeout_ms' => 3000,
]);
```

无参数的 `RiakFileServer::getInstance()` 继续复用默认单例。传入主机/端口或非空配置数组时，
`getInstance()` 每次创建独立实例，显式配置不会被已经创建的默认单例忽略，也不会改变默认连接。
`RiakFileServer`、`MemberAvatarService`、`UserRegisterService` 可传 `['client' => $client]` 注入客户端（测试用）。

`RiakFileServer`、上传控制器、头像和注销服务、Fuel 的 `fileserver` / `fileservernvf` 均直接使用 `RiakClient`。文件命名和现有 JSON 返回结构由 PHP 层保留。为延续旧实现，`RiakClient` 在每个实例首次写入某个 bucket 前设置一次 `allow_mult=false`；这是对该 bucket 共享属性的修改，原生 `riak_pb_put()` 本身不会自动修改属性。

## 原生函数

所有函数均在全局命名空间。网络、服务端或协议错误抛出异常，参数类型或值错误由 PHP 报错；调用方可使用 `catch (\Throwable $e)` 处理。

| 签名 | 返回内容 |
| --- | --- |
| `riak_pb_ping(string $host, int $port = 8129, int $timeout_ms = 3000): bool` | 成功为 `true` |
| `riak_pb_server_info(string $host, int $port = 8129, int $timeout_ms = 3000): array` | `node`、`server_version` |
| `riak_pb_get(string $host, int $port, string $bucket, string $key, int $timeout_ms = 3000): ?array` | `content`、`content_type`；不存在或为删除标记时返回 `null` |
| `riak_pb_put(string $host, int $port, string $bucket, string $key, string $content, string $content_type = 'application/octet-stream', int $timeout_ms = 3000): bool` | 成功为 `true`，按指定 key 写入 |
| `riak_pb_delete(string $host, int $port, string $bucket, string $key, int $timeout_ms = 3000): bool` | 成功为 `true` |
| `riak_pb_set_bucket_allow_mult(string $host, int $port, string $bucket, bool $allow_mult, int $timeout_ms = 3000): bool` | 成功为 `true`，修改 bucket 属性 |

先在能访问目标 Riak 的环境执行只读检查；以下示例不会上传或删除文件：

```php
<?php
declare(strict_types=1);

$host = getenv('RIAK_PB_HOST');
if ($host === false || $host === '') {
    throw new RuntimeException('请先设置 RIAK_PB_HOST');
}
$port = (int) (getenv('RIAK_PB_PORT') ?: 8129);
var_export(riak_pb_server_info($host, $port, 3000));
```

下面是可选的真实读写检查，**执行会创建一个唯一测试对象，并在结束时尝试删除它**。在确认目标主机和 bucket 后再执行；这段示例未自动运行：

```php
<?php
declare(strict_types=1);

$host = getenv('RIAK_PB_HOST');
if ($host === false || $host === '') {
    throw new RuntimeException('请先设置 RIAK_PB_HOST');
}
$port = (int) (getenv('RIAK_PB_PORT') ?: 8129);
$bucket = 'cdm';
$key = 'riak_pb_test_' . bin2hex(random_bytes(16)) . '.bin';
$content = "Riak PB test\0binary\xff";

try {
    riak_pb_put($host, $port, $bucket, $key, $content, 'application/octet-stream', 3000);
    $object = riak_pb_get($host, $port, $bucket, $key, 3000);
    if ($object === null || $object['content'] !== $content
        || $object['content_type'] !== 'application/octet-stream') {
        throw new RuntimeException('Riak 读回内容不一致');
    }
    echo "上传和读取成功：{$key}\n";
} finally {
    try {
        riak_pb_delete($host, $port, $bucket, $key, 3000);
    } catch (Throwable $cleanupError) {
        fwrite(STDERR, "测试对象清理失败：{$key}，{$cleanupError->getMessage()}\n");
    }
}
```

## 当前范围与限制

- 请求和响应的 PB 帧限制为 **64 MiB**，包含消息码及 protobuf 内容；文件可用大小还要扣除 bucket、key、MIME 等编码开销。当前为整帧缓冲，不提供大文件流式传输。
- 仅支持明文 TCP，无 TLS、证书或用户名密码认证。
- 每次原生函数请求新建 TCP 连接，完成后关闭；不使用连接池，也不自动重试。写入超时可能发生在服务端完成写入之后。
- `timeout_ms` 用于单次请求的连接和读写截止时间。DNS 解析由系统 resolver 同步完成，不受套接字截止时间的硬限制。PHP 适配器首次上传包含属性设置和上传两个请求，各自计时。
- 读取遇到多个 siblings 时抛出异常，不任意选取其中一个；需先处理对象冲突。
- 只覆盖普通 bucket 的文件操作，不包含二级索引、MapReduce、搜索或旧 PECL Riak 全部类接口。
- `RiakFileServer` 返回的 URL 仍默认使用公司文件下载域名。PB 上传成功不会创建 HTTP 下载入口，该域名必须已有可用的下载链路。

协议参考：[Riak PB 消息定义](https://github.com/basho/riak_pb)。修改协议处理后，应先通过本地测试，再针对实际 Riak 2.0.0 完成只读和隔离测试对象的联调。
