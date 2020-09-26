/*******************************************************************
 *
 *   功能： 高亮查看日志
 *
 *   使用： ./logview [options] {log_file_name}
 *
*******************************************************************/
#include <signal.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <chrono>
#include <thread>
#include <string.h>
#include <fstream>
#include <ctime>
#include <map>
#include <list>
#include <getopt.h>

void printUsage();
void printDefaultConfig();
void catchSigINT(int a);
bool readConfig(const char* cfgfile);
void start();

struct Color {
    Color() {}
    Color(int r, int g, int b) : r(r), g(g), b(b) {}
    int r = 0;
    int g = 0;
    int b = 0;
};

struct Level {
    Level(const std::string& group, const std::string& text, const Color& color)
        : groupName(group), textInLogFile(text), color(color) {}
    std::string groupName;      // 在配置文件中的组名
    std::string textInLogFile;  // 在日志文件中的字符串 ，如 "... [debug] ..."
    Color color;
};

struct Config {
    int   detectInterval = 10;     // 检测文件内容更新的间隔（10ms）
    int   lineMaxLength = 500;      // 默认一行最多500个字符
    int   linesOfLast = 20;         // 默认显示日志文件最后20行
    bool  highlightLine = false;    // 默认只高亮日志级别 (<==>) 高亮整行
    bool  showLineNumber = false;   // 默认不显示行号
    Color lineNumberColor = { 175, 95, 0 }; // 行号颜色
    Level levels[6] = {
        { "trace",    "[trace]",    { 80,  220, 44  } },
        { "debug",    "[debug]",    { 90,  220, 200 } },
        { "info",     "[info]",     { 50,  150, 240 } },
        { "warning",  "[warning]",  { 220, 240, 25  } },
        { "error",    "[error]",    { 233, 20,  20  } },
        { "critical", "[critical]", { 240, 20,  200 } }
    };
};

#define NUM_MAX_LINES 512       // 初始时最多显示512行

FILE*  g_fp = nullptr;
char*  g_log_file = nullptr;
long   g_curr_line_num = 0;
Config g_config;


/*********************************************************************
 *
 *                      main()
 *
*********************************************************************/

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage();
        return 1;
    }

    struct option opts[] = {
        { "help",       no_argument,       0, 'h' },
        { "config",     required_argument, 0, 'c' },
        { "lines",      required_argument, 0, 'n' },
        { "mode",       required_argument, 0, 'm' },
        { "linenumber", no_argument,       0, 'l' },
        { "interval",   required_argument, 0, 'i' },
        { 0, 0, 0, 0 }
    };

    // 配置文件
    const char* configFileName = nullptr;

    // 处理选项参数
    int opt_index = 0;
    int ret = 0;
    while ((ret = getopt_long(argc, argv, "n:m:i:lhc:", opts, &opt_index)) != -1) {
        switch(ret) {
        case 'c':
            configFileName = optarg;
            break;
        case 'l':
            g_config.showLineNumber = true;
            break;
        case 'i':
            g_config.detectInterval = std::atoi(optarg);
        case 'n':
            g_config.linesOfLast = std::atoi(optarg);
            break;
        case 'm':
            if (strcmp(optarg, "default") == 0 ||
                strcmp(optarg, "keyword") == 0 ||
                strcmp(optarg, "level") == 0)
            {
                g_config.highlightLine = false;
            }
            else if (strcmp(optarg, "line") == 0) {
                g_config.highlightLine = true;
            }
            else {
                printf("Invalid param of option --mode(-m)\n");
                return 1;
            }
            break;
        case 'h':
        case '?':
        default:
            printUsage();
            return 1;
        }
    }

    // 处理非选项参数
    //  只有一个，即日志文件名
    if (optind + 1 == argc) {
        g_log_file = argv[optind];
    }
    else {
        printUsage();
        return 1;
    }

    // 注册捕获SIGINT信号的函数
    signal(SIGINT, catchSigINT);

    if (configFileName && !readConfig(configFileName)) {
        return 1;
    }

    if (g_config.detectInterval < 0) {
        g_config.detectInterval = 0;
    }
    if (g_config.linesOfLast < 0) {
        g_config.linesOfLast = 0;
    }
    if (g_config.lineMaxLength > NUM_MAX_LINES) {
        printf("Error: show %d lines at most.\n", NUM_MAX_LINES);
        return 1;
    }

    start();
}


/*****************************************
 *
 *    辅助函数
 *
*****************************************/

// 捕获SIGINT信号 
void catchSigINT(int a) {
    if (g_fp) {
        fclose(g_fp);
        g_fp = nullptr;
    }
    exit(1);
}

void printUsage() {
    printf("--------------------\n");
    printf("Usage:\n"); 
    printf("--------------------\n");
    printf("    logview [options] filename\n");
    printf("\n");
    printf("--------------------\n");
    printf("Options:\n");
    printf("--------------------\n");
    printf("    -h --help                    show help information.\n");
    printf("    -c --config {cfgFileName}    load config from file\n");
    printf("    -i --interval {milliseconds} interval of detecting new content\n");
    printf("    -l --linenumber              show line number.\n");
    printf("    -n --lines {number}          lines of last to show.\n");
    printf("    -m --mode {mode}             highlight mode: line/keyword.\n");
    printf("\n");
    printf("--------------------\n");
    printf("Default config:\n");
    printf("--------------------\n");
    printDefaultConfig();
}


/*************************************************************
 *
 *   日志显示函数
 *
*************************************************************/

// 彩色输出一行内容
void printColorfulLine(const char* line, int len, long lineNumber) {
    // 去除最后面的换行符
    while (line[len-1] == '\n' || line[len-1] == '\r') {
        --len;
    }

    // 多30个字符，用于添加改变颜色的字符
    char* lineBuff = new char[len + 40];
    int lineOffset = 0;
    int lineBuffOffset = 0;

    // 是否含有日志级别关键词
    const char* highlightStart = nullptr;
    for (int i = 0; i < 6; ++i) {
        Level& level = g_config.levels[i];
        // 高亮 起始位置
        highlightStart = strstr(line, level.textInLogFile.c_str());
        if (!highlightStart) {
            continue;
        }

        // 高亮一整行
        if (g_config.highlightLine) {
            // 改变输出颜色
            lineBuffOffset += sprintf(lineBuff, "\033[38;2;%d;%d;%dm",
                                      level.color.r, level.color.g, level.color.b);
            // 拼接 日志内容
            strncpy(lineBuff + lineBuffOffset, line, len);
            lineBuffOffset += len;
            // 恢复默认颜色
            lineBuffOffset += sprintf(lineBuff + lineBuffOffset, "\033[0m");
        }
        // 只高亮日志级别
        else {
            int lenBeforeHighlight = highlightStart - line;

            // 在【日志级别】前面的内容
            strncpy(lineBuff, line, lenBeforeHighlight);
            lineOffset     += lenBeforeHighlight;
            lineBuffOffset += lenBeforeHighlight;

            // 改变输出颜色
            lineBuffOffset += sprintf(lineBuff + lineBuffOffset, "\033[38;2;%d;%d;%dm",
                                      level.color.r, level.color.g, level.color.b);

            // 拼接 日志级别
            strncpy(lineBuff + lineBuffOffset, level.textInLogFile.c_str(),
                    level.textInLogFile.length());
            lineOffset     += level.textInLogFile.length();
            lineBuffOffset += level.textInLogFile.length();

            // 恢复默认颜色
            lineBuffOffset += sprintf(lineBuff + lineBuffOffset, "\033[0m");

            // 剩余的字符: 【日志级别】之后的字符串
            int lenLeft = len - lineOffset;
            strncpy(lineBuff + lineBuffOffset, line + lineOffset, lenLeft);
            lineOffset     += lenLeft;
            lineBuffOffset += lenLeft;
        }

        break;
    }

    // 显示行号
    if (g_config.showLineNumber) {
        printf("\033[38;2;%d;%d;%dm%ld\033[0m ",
               g_config.lineNumberColor.r,
               g_config.lineNumberColor.g,
               g_config.lineNumberColor.b,
               lineNumber);
    }

    // 该行没有日志级别，直接原样输出即可
    if (!highlightStart) {
        strncpy(lineBuff, line, len);
        lineBuffOffset += len;
    }

    lineBuff[lineBuffOffset] = '\0';
    printf("%s\n", lineBuff);
    delete[] lineBuff;
}


// 打印文件最后几行
int printLastLines(int lines) {
    g_fp = fopen(g_log_file, "r");
    if (!g_fp) {
        printf("Open log file:[%s] failed!\n", g_log_file);
        exit(1);
    }

    fseek(g_fp, 0, SEEK_END);
    long fileSize = ftell(g_fp);
    fseek(g_fp, 0, SEEK_SET);

    if (lines == 0) {
        fclose(g_fp);
        g_fp = nullptr;
        return fileSize;
    }

    // list链表
    // 该行的缓冲区指针 ，行号
    std::list<std::pair<char*, long>> lineBuffList;

    g_curr_line_num = 0;
    while (!feof(g_fp)) {
        char* lineBuff = new char[g_config.lineMaxLength + 1];
        fgets(lineBuff, g_config.lineMaxLength, g_fp);
        if (feof(g_fp)) {  // important on Linux platform
            break;
        }
        ++g_curr_line_num;
        lineBuffList.emplace_back(lineBuff, g_curr_line_num);
        // 如果已经读取够了指定行数
        // 就进行滚动覆盖前面的内容
        // 最后剩下的就是需要打印的日志
        if (g_curr_line_num > lines) {
            delete lineBuffList.front().first;
            lineBuffList.pop_front();
        }
    }

    for (auto it = lineBuffList.begin(); it != lineBuffList.end(); ++it) {
        printColorfulLine(it->first, strlen(it->first), it->second);
        delete(it->first);
    }

    fclose(g_fp);
    g_fp = nullptr;
    return fileSize;
}


void start() {
    printf("------------------------------------------------------------\n");
    printf("                       logview                              \n");
    printf("------------------------------------------------------------\n");

    // 打印最后几行行
    int fileSize = printLastLines(g_config.linesOfLast);

    int lenLast = fileSize;  // 上一次文件长度
    int lenThis = fileSize;  // 当前文件长度

    // 每隔10ms读取一次文件， 如果有新内容就输出
    while (true) {
        g_fp = fopen(g_log_file, "r");
        if (!g_fp) {
            printf("Open log file: %s failed.\n", g_log_file);
            fclose(g_fp);
            g_fp = nullptr;
            exit(1);
        }

        fseek(g_fp, 0, SEEK_END);
        lenThis = ftell(g_fp);
        
        // 两次文件长度不一致
        // 文件内容发生变化
        if (lenThis < lenLast) {
            // critical error
            printf("Critical error: lenThis < lenLast\n");
            fclose(g_fp);
            g_fp = nullptr;
            exit(1);
        }
        if (lenThis > lenLast) {
            // 直接定位到上一次的文件末尾
            long needToRead = lenThis - lenLast;
            fseek(g_fp, -needToRead, SEEK_END);
            char* lineBuff = new char[g_config.lineMaxLength + 1];
            while (!feof(g_fp)) {
                fgets(lineBuff, g_config.lineMaxLength, g_fp);
                if (feof(g_fp)) {  // important on Linux platform
                    break;
                }
                ++g_curr_line_num;
                printColorfulLine(lineBuff, strlen(lineBuff), g_curr_line_num);
            }
            delete[] lineBuff;
            lenLast = lenThis;
        }

        fclose(g_fp);
        g_fp = nullptr;
        std::this_thread::sleep_for(std::chrono::milliseconds(g_config.detectInterval));
    }
}


/***********************************************************
 *
 *   读取配置文件
 *
***********************************************************/

// 去除左右的空格
std::string& trim(std::string& str) {
    str.erase(0, str.find_first_not_of(" "));
    str.erase(str.find_last_not_of(" ") + 1);
    return str;
}

// 替换所有字符串
std::string& replaceAll(std::string& str,
                         const std::string& oldVal,
                         const std::string& newVal)
{
    while(true) {
        std::string::size_type pos(0);
        if ((pos = str.find(oldVal)) != std::string::npos){
            str.replace(pos,oldVal.length(),newVal);
        }
        else {
            return str;
        }
    }
}

// 解析颜色值
bool parseColor(const std::string& value, Color& color) {
    auto pos1 = value.find(",");
    if (pos1 == std::string::npos) {
        printf("Ivalid color: %s\n", value.c_str());
        return false;
    }
    int r = std::atoi(value.substr(0, pos1).c_str());
    auto pos2 = value.find(",", pos1 + 1);
    if (pos2 == std::string::npos) {
        printf("Ivalid color: %s\n", value.c_str());
        return false;
    }
    int g = std::atoi(value.substr(pos1 + 1, pos2 - pos1 - 1).c_str());
    int b = std::atoi(value.substr(pos2 + 1).c_str());
    if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) {
        printf("Ivalid color: %s\n", value.c_str());
        return false;
    }
    color = { r, g, b };
    return true;
}

// 解析Key-Value配置项
bool parseKeyValueOfConfig(const std::string& currGroup,
                           const std::string& key,
                           const std::string& value)
{
    if (key.empty() || value.empty()) {
        printf("Group [%s]: invalid key-value: %s=%s\n", currGroup.c_str(),
               key.c_str(), value.c_str());
        return false;
    }

    for (int i = 0; i < 6; ++i) {
        Level& level = g_config.levels[i];
        if (currGroup == level.groupName) {
            if (key == "color") {
                return parseColor(value, level.color);
            }
            else if (key == "text") {
                level.textInLogFile = value;
                return true;
            }
            else {
                printf("Group [%s]: invalid key-value: %s=%s\n", currGroup.c_str(),
                       key.c_str(), value.c_str());
                return false;
            }
        }
    }

    if (currGroup == "basic") {
        if (key == "line_max_length") {
            g_config.lineMaxLength = std::atoi(value.c_str());
            return true;
        }
        else if (key == "lines_of_last") {
            g_config.linesOfLast = std::atoi(value.c_str());
            return true;
        }
        else if (key == "detect_interval") {
            g_config.detectInterval = std::atoi(value.c_str());
            return true;
        }
        else if (key == "highlight_line") {
            if (value == "true" || value == "1") {
                g_config.highlightLine = true;
                return true;
            }
            else if (key == "false" || value == "0") {
                g_config.highlightLine = false;
                return true;
            }
        }
        else if (key == "show_line_number") {
            if (value == "true" || value == "1") {
                g_config.showLineNumber = true;
                return true;
            }
            else if (key == "false" || value == "0") {
                g_config.showLineNumber = false;
                return true;
            }
        }
        else if (key == "line_number_color") {
            return parseColor(value, g_config.lineNumberColor);
        }
    }

    printf("Group [%s]: invalid key-value: %s=%s\n", currGroup.c_str(),
           key.c_str(), value.c_str());
    return false;
}

// 传入当前group名称
// 返回下一组group名称（为空，说明到达文件结尾
bool parseGroup(std::ifstream& ifs, const std::string& currGroup) {
    while (!ifs.eof()) {
        std::string line;
        std::getline(ifs, line);
        if (ifs.eof()) {
            break;
        }
        if (trim(line).empty() || line.front() == '#') {
            continue;
        }
        if (line.length() < 3) {
            printf("Invalid line: %s\n", line.c_str());
            return false;
        }
        // 下一组配置项
        if (line.front() == '[' && line.back() == ']') {
            // 递归
            std::string nextGroup = line.substr(1, line.length() - 2);
            return parseGroup(ifs, trim(nextGroup));
        }
        // 组内的配置项
        auto pos = line.find("=");
        if (pos == std::string::npos) {
            printf("Invalid line: %s\n", line.c_str());
            return false;
        }
        std::string key = line.substr(0, pos);
        std::string value = line.substr(pos + 1);
        trim(key);
        replaceAll(trim(value), "<space>", " ");
        if (key.empty() || value.empty()) {
            printf("Invalid line: %s\n", line.c_str());
            return false;
        }
        if (!parseKeyValueOfConfig(currGroup, key, value)) {
            return false;
        }
    }
    return true;
}

bool readConfig(const char* cfgFile) {
    std::ifstream ifs(cfgFile);    
    if (!ifs) {
        printf("Read config failed: %s.\n", cfgFile);
        return false;
    }

    char line[512] = { 0 };
    std::string currGroup;   // 当前组名
    while (!ifs.eof()) {
        std::string line;
        std::getline(ifs, line);
        if (ifs.eof()) {
            break;
        }
        if (trim(line).empty() || line.front() == '#') {
            continue;
        }
        if (line.length() < 2 || line.front() != '[' || line.back() != ']') {
            printf("Invalid line: %s\n", line.c_str());
            return false;
        }

        currGroup = line.substr(1, line.length() - 2);
        if (!parseGroup(ifs, trim(currGroup))) {
            return false;
        }
    }

    return true;
}

void printDefaultConfig() {
    printf("[basic]\n");
    printf("detect_interval=10ms\n");
    printf("lines_of_last=20\n");
    printf("line_max_length=500\n");
    printf("highlight_line=false\n");
    printf("show_line_number=false\n");
    printf("line_number_color=175,95,0\n\n");

    printf("[trace]\n");
    printf("text=[trace]\n");
    printf("color=80,220,44\n\n");

    printf("[debug]\n");
    printf("text=[debug]\n");
    printf("color=90,220,200\n\n");

    printf("[info]\n");
    printf("text=[info]\n");
    printf("color=50,150,240\n\n");

    printf("[warning]\n");
    printf("text=[warning]\n");
    printf("color=220,240,25\n\n");

    printf("[error]\n");
    printf("text=[error]\n");
    printf("color=2330,20,20\n\n");

    printf("[critical]\n");
    printf("text=[critical]\n");
    printf("color=240,20,200\n");
}

