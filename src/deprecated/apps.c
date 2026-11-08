/*
 * apps.c - 服务识别实现
 *
 * 表是数据不是逻辑，新增服务只要加一行，不需要动任何匹配代码。
 * 顺序即优先级：越具体的放前面（例如 ys7.com 要在 cdn 之前）。
 */
#include "apps.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

typedef struct {
    const char *keys;    /* 逗号分隔的小写关键词 */
    const char *name;
    const char *cat;
} sig_t;

static const sig_t SIGS[] = {
    /* ---- 短视频 / 直播平台 ---- */
    {"douyin,snssdk,amemv,byteoversea,bytecdn,ixigua,toutiao,pstatp,zijieapi,mssdk,volces,tiktok,byteimg",
     "抖音 / 字节", "短视频"},
    {"kuaishou,kwai,gifshow,ksapisrv,yximg,kuaishoucdn", "快手", "短视频"},
    {"xiaohongshu,xhscdn,xhsapi", "小红书", "短视频"},
    {"bilibili,biliapi,hdslb,bilivideo,bilicdn,acgvideo", "哔哩哔哩", "视频"},
    {"iqiyi,qiyi,pps.tv,qiyipic", "爱奇艺", "长视频"},
    {"youku,ykimg,tudou,alikunlun", "优酷", "长视频"},
    {"qqlive,v.qq.com,gtimg,live.qq,qqvideo", "腾讯视频", "长视频"},
    {"mgtv,hunantv,imgo", "芒果 TV", "长视频"},
    {"rtmp,flv,hls,.live.,livecdn,pushcdn", "直播流", "直播/监控"},

    /* ---- 监控 / NAS / 物联网 ---- */
    {"ys7,ezviz,hikvision,ezvizlife", "萤石云 / 监控", "监控"},
    {"synology,ugnas,ugreen,qnap,dsm.", "NAS / 私有云", "存储备份"},
    {"mi.com,xiaomi,miui,xiaomi.net,mijia", "小米 / 米家", "IoT"},
    {"sctel,10086.cn,chinamobile", "运营商 IoT", "IoT"},
    {"mqtt,airmx,roborock,midea,phicomm,qingping", "智能设备", "IoT"},

    /* ---- 下载 / P2P / 更新 ---- */
    {"tracker,torrent,pt.bt,btschool,transmission,qbittorrent", "PT / BT", "P2P 下载"},
    {"windowsupdate,microsoft,update.microsoft,download.windows", "Windows 更新", "系统更新"},
    {"apple,icloud,itunes,appstore,mesu.apple,softwareupdate", "Apple 服务", "系统更新"},
    {"googleapis,gstatic,google.com,android.clients,play.googleapis", "Google 服务", "系统更新"},
    {"synology.com,pkgupdate,download.synology", "系统 / 固件更新", "系统更新"},
    {"cdn,download,dl.,mirror,apt,yum,pypi,npm,maven,github-releases,objects.githubusercontent",
     "下载 / CDN", "下载"},
    {"steam,steamcontent,epicgames,blizzard,origin.com", "游戏下载", "下载"},

    /* ---- 社交 / 通讯 / 购物 ---- */
    {"weixin,wechat,mmbiz,wx.qq.com,qq.com,tenpay", "微信 / QQ", "社交"},
    {"taobao,alicdn,alipay,tmall,alibaba", "淘宝 / 阿里", "购物"},
    {"jd.com,360buy,jdcloud", "京东", "购物"},
    {"baidu,bdstatic,bdurl", "百度", "搜索"},
    {"workbuddy", "WorkBuddy", "办公"},
    {"cloudflare,cf.,1.1.1.1,dns.google", "网络基础设施", "网络服务"},

    /* ---- 遥测 / 广告（多数已被拦截，仍可用于识别） ---- */
    {"gdt.qq.com,ugdtimg,mdt.qq.com,beacon,adsmind,ctobsnssdk,datadoghq,umeng,talkingdata",
     "广告 / 遥测", "遥测"},
};

typedef struct {
    int port;
    const char *name;
    const char *cat;
} port_sig_t;

static const port_sig_t PORTS[] = {
    { 1935, "RTMP 直播推流", "直播/监控" },
    { 554,  "RTSP 视频流",   "直播/监控" },
    { 8554, "RTSP 视频流",   "直播/监控" },
    { 6881, "BitTorrent",    "P2P 下载" },
    { 6882, "BitTorrent",    "P2P 下载" },
    { 6889, "BitTorrent",    "P2P 下载" },
    { 51413,"BitTorrent",    "P2P 下载" },
    { 1194, "OpenVPN",       "VPN" },
    { 51820,"WireGuard",     "VPN" },
    { 5228, "推送通知",       "系统推送" },
    { 123,  "NTP 时间同步",   "网络服务" },
    { 53,   "DNS",           "网络服务" },
    { 443,  "HTTPS",         "网页" },
    { 80,   "HTTP",          "网页" },
    { 22,   "SSH",           "远程" },
    { 3389, "远程桌面",       "远程" },
};

/* 大小写不敏感的子串匹配 */
static int has_ci(const char *s, const char *sub)
{
    if (!s || !sub || !*sub) return 0;
    size_t n = strlen(sub);
    for (; *s; s++) {
        size_t i = 0;
        while (i < n && s[i] &&
               tolower((unsigned char)s[i]) == tolower((unsigned char)sub[i])) i++;
        if (i == n) return 1;
    }
    return 0;
}

static int key_match(const char *host, const char *keys)
{
    const char *p = keys;
    while (*p) {
        const char *e = strchr(p, ',');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        char word[64];
        if (n < sizeof(word)) {
            memcpy(word, p, n);
            word[n] = 0;
            if (has_ci(host, word)) return 1;
        }
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

int apps_classify(const char *host, int dport, char *name, size_t n,
                  const char **cat)
{
    if (cat) *cat = "其他";
    if (name && n) name[0] = 0;

    /* 1) 域名关键词优先 */
    if (host && host[0]) {
        for (size_t i = 0; i < sizeof(SIGS) / sizeof(SIGS[0]); i++) {
            if (!key_match(host, SIGS[i].keys)) continue;
            if (name && n) snprintf(name, n, "%s", SIGS[i].name);
            if (cat) *cat = SIGS[i].cat;
            return 1;
        }
    }
    /* 2) 没有域名（IP 直连）或没命中，用端口兜底 */
    for (size_t i = 0; i < sizeof(PORTS) / sizeof(PORTS[0]); i++) {
        if (PORTS[i].port != dport) continue;
        if (name && n) snprintf(name, n, "%s", PORTS[i].name);
        if (cat) *cat = PORTS[i].cat;
        return 1;
    }
    if (name && n) snprintf(name, n, "其他");
    return 0;
}

/* 与「内置组件后台上报」相关的域名特征。
   这些是线索不是证据：正常 App 的日志上报也会命中，界面必须标注需人工核实。 */
typedef struct { const char *key; const char *kind; } tel_t;
static const tel_t TEL[] = {
    { "wo.com.cn",        "运营商上报端点" },
    { "guardprovider",    "系统安全组件" },
    { "antifraud",        "反诈检测" },
    { "fraud",            "反诈检测" },
    { "screenrecord",     "录屏相关" },
    { "screencap",        "截屏相关" },
    { "clip.mp4",         "视频切片" },
    { "telemetry",        "遥测" },
    { "diagnostics",      "诊断上报" },
    { "crashlytics",      "崩溃上报" },
    { "apmplus",          "性能监控" },
    { "beacon",           "行为打点" },
    { "sdklog",           "SDK 日志" },
    { "analytics",        "行为分析" },
    { "upload",           "上传端点" },
};

int apps_is_telemetry(const char *qname, char *kind, size_t n)
{
    if (!qname || !*qname) return 0;
    for (size_t i = 0; i < sizeof(TEL) / sizeof(TEL[0]); i++) {
        if (!has_ci(qname, TEL[i].key)) continue;
        if (kind && n) snprintf(kind, n, "%s", TEL[i].kind);
        return 1;
    }
    return 0;
}
