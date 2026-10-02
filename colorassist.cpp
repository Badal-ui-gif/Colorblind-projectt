// Color Assistant 2.0
//   * Main menu: Check an image / Use the webcam / Colorblind test
//   * Color checker works on a picked file (native file dialog) or a live webcam feed
//   * Exact color codes, palette, colorblind simulation + enhancement, "may look alike" warnings
//   * Optional AI description through a free local Ollama model (needs a vision model, e.g. llava)
//   * Colorblind test: 8 generated Ishihara-style dot plates, on-screen keypad, scored results
//
// Run:  ./colorassist [image.jpg] [ollama_model]        (default model: llava)
//
// Build (Linux/macOS example):
//   g++ -std=c++17 -O2 colorassist.cpp -o colorassist $(pkg-config --cflags --libs opencv4) -lcurl
// Windows: link opencv_world, libcurl and comdlg32 (MSVC links comdlg32 automatically).
// File dialog on Linux uses 'zenity' (or 'kdialog'): sudo apt install zenity

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include <opencv2/opencv.hpp>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#ifdef _MSC_VER
#pragma comment(lib, "comdlg32.lib")
#endif
#endif
using namespace cv;
using namespace std;
using json = nlohmann::json;

// ============================================================ layout + theme
static const int WW = 1200, WH = 720, TOP = 56, PNL_X = 800, PNL_W = 400, STRIP = 120;

static Scalar rgb(int r, int g, int b) { return Scalar(b, g, r); }
static const Scalar C_BG = rgb(18, 20, 26), C_PANEL = rgb(28, 31, 39), C_CARD = rgb(40, 44, 56),
                    C_HOT = rgb(54, 60, 80), C_LINE = rgb(78, 84, 104), C_ACC = rgb(59, 130, 246),
                    C_ACC2 = rgb(245, 166, 35), C_TXT = rgb(236, 239, 246), C_DIM = rgb(150, 156, 172),
                    C_GOOD = rgb(72, 199, 116), C_BAD = rgb(240, 84, 84);

enum Screen { S_MENU, S_CHECK, S_TEST };

// ============================================================ tiny immediate-mode UI
struct UiState { int mx = -1, my = -1; bool down = false, click = false; int wheel = 0; };
static UiState UI;
static string g_notice;
static int g_noticeTtl = 0;
void notify(const string& s) { g_notice = s; g_noticeTtl = 140; }

void onMouse(int ev, int x, int y, int flags, void*) {
    UI.mx = x; UI.my = y;
    if (ev == EVENT_LBUTTONDOWN) { UI.down = true; UI.click = true; }
    else if (ev == EVENT_LBUTTONUP) UI.down = false;
    else if (ev == EVENT_MOUSEWHEEL) UI.wheel += getMouseWheelDelta(flags);
}
bool hot(const Rect& r) { return r.contains(Point(UI.mx, UI.my)); }

void fillRound(Mat& m, Rect r, int rad, const Scalar& c) {
    rad = min(rad, min(r.width, r.height) / 2);
    rectangle(m, Rect(r.x + rad, r.y, r.width - 2 * rad, r.height), c, FILLED);
    rectangle(m, Rect(r.x, r.y + rad, r.width, r.height - 2 * rad), c, FILLED);
    circle(m, Point(r.x + rad, r.y + rad), rad, c, FILLED, LINE_AA);
    circle(m, Point(r.x + r.width - rad - 1, r.y + rad), rad, c, FILLED, LINE_AA);
    circle(m, Point(r.x + rad, r.y + r.height - rad - 1), rad, c, FILLED, LINE_AA);
    circle(m, Point(r.x + r.width - rad - 1, r.y + r.height - rad - 1), rad, c, FILLED, LINE_AA);
}
void txt(Mat& m, const string& s, Point p, double sc, const Scalar& c, int th = 1) {
    putText(m, s, p, FONT_HERSHEY_SIMPLEX, sc, c, th, LINE_AA);
}
Size tsz(const string& s, double sc, int th = 1) {
    int bl;
    return getTextSize(s, FONT_HERSHEY_SIMPLEX, sc, th, &bl);
}
void txtC(Mat& m, const string& s, Rect r, double sc, const Scalar& c, int th = 1) {
    Size z = tsz(s, sc, th);
    txt(m, s, Point(r.x + (r.width - z.width) / 2, r.y + (r.height + z.height) / 2), sc, c, th);
}
bool button(Mat& m, Rect r, const string& label, bool active = false, bool enabled = true, double sc = 0.5) {
    bool h = enabled && hot(r);
    fillRound(m, r, 9, active ? C_ACC : (h ? C_ACC : C_LINE));
    fillRound(m, Rect(r.x + 1, r.y + 1, r.width - 2, r.height - 2), 8, active ? C_ACC : (h ? C_HOT : C_CARD));
    txtC(m, label, r, sc, enabled ? C_TXT : C_DIM);
    if (h && UI.click) { UI.click = false; return true; }
    return false;
}

// ============================================================ color names / conversions
struct NC { const char* name; int r, g, b; };
static const NC NAMED[] = {
    {"Black", 0, 0, 0}, {"Charcoal", 54, 69, 79}, {"Dark Gray", 85, 85, 85}, {"Gray", 128, 128, 128},
    {"Silver", 192, 192, 192}, {"Light Gray", 220, 220, 220}, {"White", 255, 255, 255}, {"Ivory", 255, 255, 240},
    {"Red", 230, 25, 25}, {"Crimson", 220, 20, 60}, {"Dark Red", 139, 0, 0}, {"Maroon", 110, 15, 40},
    {"Coral", 255, 127, 80}, {"Salmon", 250, 128, 114}, {"Pink", 255, 192, 203}, {"Hot Pink", 255, 105, 180},
    {"Magenta", 255, 0, 255}, {"Orange", 255, 165, 0}, {"Peach", 255, 218, 185}, {"Gold", 255, 215, 0},
    {"Yellow", 255, 240, 0}, {"Light Yellow", 255, 255, 153}, {"Mustard", 225, 173, 1}, {"Olive", 128, 128, 0},
    {"Lime", 170, 255, 0}, {"Light Green", 144, 238, 144}, {"Green", 0, 160, 0}, {"Forest Green", 34, 120, 34},
    {"Dark Green", 0, 80, 20}, {"Teal", 0, 128, 128}, {"Turquoise", 64, 224, 208}, {"Cyan", 0, 255, 255},
    {"Sky Blue", 135, 206, 235}, {"Light Blue", 173, 216, 230}, {"Blue", 0, 0, 255}, {"Royal Blue", 65, 105, 225},
    {"Steel Blue", 70, 130, 180}, {"Navy", 0, 0, 110}, {"Purple", 128, 0, 128}, {"Violet", 200, 130, 238},
    {"Lavender", 230, 230, 250}, {"Indigo", 75, 0, 130}, {"Plum", 142, 69, 133}, {"Brown", 139, 69, 19},
    {"Chocolate", 90, 50, 20}, {"Rust", 183, 65, 14}, {"Tan", 210, 180, 140}, {"Beige", 245, 245, 220},
    {"Khaki", 195, 176, 145}, {"Sand", 194, 178, 128}, {"Cream", 255, 253, 208}, {"Skin Tone", 224, 172, 105},
    {"Burnt Orange", 204, 85, 0}, {"Carrot Orange", 237, 145, 33}, {"Amber", 255, 191, 0}};
static const int NNAMED = sizeof(NAMED) / sizeof(NAMED[0]);

Vec3f toLab(const Vec3f& bgr) {
    Mat px(1, 1, CV_32FC3, Scalar(bgr[0] / 255.f, bgr[1] / 255.f, bgr[2] / 255.f));
    Mat lab;
    cvtColor(px, lab, COLOR_BGR2Lab);
    return lab.at<Vec3f>(0, 0);
}
string colorName(const Vec3f& bgr) {
    static vector<Vec3f> labs;
    if (labs.empty())
        for (int i = 0; i < NNAMED; i++) labs.push_back(toLab(Vec3f(NAMED[i].b, NAMED[i].g, NAMED[i].r)));
    Vec3f l = toLab(bgr);
    int best = 0;
    double bd = 1e18;
    for (int i = 0; i < NNAMED; i++) {
        double d = norm(l - labs[i]);
        if (d < bd) { bd = d; best = i; }
    }
    return NAMED[best].name;
}
string lightnessWord(const Vec3f& bgr) {
    float L = toLab(bgr)[0];
    return L < 25 ? "very dark" : L < 45 ? "dark" : L < 65 ? "medium" : L < 85 ? "light" : "very light";
}
string toHex(const Vec3f& c) {
    char b[8];
    snprintf(b, sizeof(b), "#%02X%02X%02X", clamp(cvRound(c[2]), 0, 255), clamp(cvRound(c[1]), 0, 255),
             clamp(cvRound(c[0]), 0, 255));
    return b;
}
string toHSV(const Vec3f& c) {
    Mat px(1, 1, CV_8UC3, Scalar(clamp(cvRound(c[0]), 0, 255), clamp(cvRound(c[1]), 0, 255), clamp(cvRound(c[2]), 0, 255)));
    Mat hsv;
    cvtColor(px, hsv, COLOR_BGR2HSV);
    Vec3b v = hsv.at<Vec3b>(0, 0);
    char b[64];
    snprintf(b, sizeof(b), "HSV %d deg, %d%%, %d%%", v[0] * 2, cvRound(v[1] / 2.55), cvRound(v[2] / 2.55));
    return b;
}
string toRGB(const Vec3f& c) {
    char b[64];
    snprintf(b, sizeof(b), "RGB %d, %d, %d", clamp(cvRound(c[2]), 0, 255), clamp(cvRound(c[1]), 0, 255),
             clamp(cvRound(c[0]), 0, 255));
    return b;
}

// ============================================================ color-blindness simulation
// Machado et al. (2009), severity 1.0, applied in linear RGB.
static const float SIMM[3][9] = {
    {0.152286f, 1.052583f, -0.204868f, 0.114503f, 0.786281f, 0.099216f, -0.003882f, -0.048116f, 1.051998f},
    {0.367322f, 0.860646f, -0.227968f, 0.280085f, 0.672501f, 0.047413f, -0.011820f, 0.042940f, 0.968881f},
    {1.255528f, -0.076749f, -0.178779f, -0.078411f, 0.930809f, 0.147602f, 0.004733f, 0.691367f, 0.303900f}};
static const char* DEF_NAMES[] = {"Protanopia (red-blind)", "Deuteranopia (green-blind)", "Tritanopia (blue-blind)"};
static const char* VIEW_NAMES[] = {"Original", "Simulated", "Enhanced"};

static float srgb2lin(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }
static float lin2srgb(float c) {
    c = clamp(c, 0.f, 1.f);
    return c <= 0.0031308f ? 12.92f * c : 1.055f * powf(c, 1.f / 2.4f) - 0.055f;
}
struct Luts {
    float fwd[256];
    uchar inv[4096];
    Luts() {
        for (int i = 0; i < 256; i++) fwd[i] = srgb2lin(i / 255.f);
        for (int i = 0; i < 4096; i++) inv[i] = (uchar)cvRound(lin2srgb(i / 4095.f) * 255.f);
    }
    uchar enc(float v) const { return inv[(int)(clamp(v, 0.f, 1.f) * 4095.f + 0.5f)]; }
};
static const Luts LUTS;

Vec3f simulatePixel(const Vec3f& bgr, int type) {
    const float* m = SIMM[type];
    float r = srgb2lin(bgr[2] / 255.f), g = srgb2lin(bgr[1] / 255.f), b = srgb2lin(bgr[0] / 255.f);
    float R = m[0] * r + m[1] * g + m[2] * b, G = m[3] * r + m[4] * g + m[5] * b, B = m[6] * r + m[7] * g + m[8] * b;
    return Vec3f(lin2srgb(B) * 255.f, lin2srgb(G) * 255.f, lin2srgb(R) * 255.f);
}
Mat simulateImage(const Mat& bgr, int type) {
    const float* m = SIMM[type];
    Mat out(bgr.size(), CV_8UC3);
    for (int y = 0; y < bgr.rows; y++) {
        const Vec3b* s = bgr.ptr<Vec3b>(y);
        Vec3b* d = out.ptr<Vec3b>(y);
        for (int x = 0; x < bgr.cols; x++) {
            float b = LUTS.fwd[s[x][0]], g = LUTS.fwd[s[x][1]], r = LUTS.fwd[s[x][2]];
            d[x] = Vec3b(LUTS.enc(m[6] * r + m[7] * g + m[8] * b), LUTS.enc(m[3] * r + m[4] * g + m[5] * b),
                         LUTS.enc(m[0] * r + m[1] * g + m[2] * b));
        }
    }
    return out;
}
// "Daltonization": shift color information the viewer cannot see into channels they can.
Mat daltonize(const Mat& bgr, int type) {
    Mat sim = simulateImage(bgr, type), out(bgr.size(), CV_8UC3);
    for (int y = 0; y < bgr.rows; y++) {
        const Vec3b* o = bgr.ptr<Vec3b>(y);
        const Vec3b* s = sim.ptr<Vec3b>(y);
        Vec3b* d = out.ptr<Vec3b>(y);
        for (int x = 0; x < bgr.cols; x++) {
            float r = o[x][2], g = o[x][1], b = o[x][0];
            float er = r - s[x][2], eg = g - s[x][1], eb = b - s[x][0];
            float nr = r, ng = g, nb = b;
            if (type < 2) { ng = g + 0.7f * er + eg; nb = b + 0.7f * er + eb; }
            else          { nr = r + er + 0.7f * eb; ng = g + eg + 0.7f * eb; }
            d[x] = Vec3b(saturate_cast<uchar>(nb), saturate_cast<uchar>(ng), saturate_cast<uchar>(nr));
        }
    }
    return out;
}

// ============================================================ text helpers
string asciiOnly(const string& s) {
    string o;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        if (c < 0x80) {
            if (c != '*' && c != '#' && c != '`') o += (char)c;
            i++;
        } else {
            int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
            if (len == 3 && i + 2 < s.size() && c == 0xE2 && (unsigned char)s[i + 1] == 0x80) {
                unsigned char t = s[i + 2];
                if (t == 0x98 || t == 0x99) o += '\'';
                else if (t == 0x9C || t == 0x9D) o += '"';
                else if (t == 0x93 || t == 0x94 || t == 0xA2) o += '-';
            }
            i += len;
        }
    }
    return o;
}
vector<string> wrapText(const string& s, int maxW, double scale) {
    vector<string> out;
    istringstream paras(s);
    string para;
    while (getline(paras, para)) {
        istringstream iss(para);
        string w, cur;
        while (iss >> w) {
            string t = cur.empty() ? w : cur + " " + w;
            if (tsz(t, scale).width > maxW && !cur.empty()) { out.push_back(cur); cur = w; }
            else cur = t;
        }
        out.push_back(cur);
    }
    return out;
}
Mat renderBlock(const string& text, int W, double scale, const Scalar& col) {
    auto lines = wrapText(text, W - 4, scale);
    int lh = cvRound(scale * 40);
    Mat m(max(lh, (int)lines.size() * lh + 6), W, CV_8UC3, C_PANEL);
    for (size_t i = 0; i < lines.size(); i++) txt(m, lines[i], Point(2, (int)(i + 1) * lh - 4), scale, col);
    return m;
}

// ============================================================ checker state + analysis
struct Cluster { Vec3f bgr; float pct; string hex, name; };
struct LabelItem { Point pt; string text; Scalar col, outline; };

struct Checker {
    Mat base, view;
    vector<Cluster> pal;
    vector<LabelItem> labels;
    vector<string> warnings;
    int viewMode = 0, defType = 1, K = 6;
    bool showLabels = true;
    bool picked = false;
    Point pick;
    Vec3f pickBgr;
    // source
    string source;
    bool live = false, frozen = false;
    int camIdx = 0, frameNo = 0;
    VideoCapture cap;
    // view geometry (canvas coordinates)
    Rect disp;
    double dscale = 1.0;
    // AI panel
    Mat aiBlock;
    int aiVer = -1, aiScroll = 0, aiMax = 0;
    bool typing = false;
    string typed;
};

Vec3f avgColor(const Mat& img, Point p) {
    Rect r(p.x - 2, p.y - 2, 5, 5);
    r &= Rect(0, 0, img.cols, img.rows);
    if (r.area() == 0) return Vec3f(0, 0, 0);
    Scalar m = mean(img(r));
    return Vec3f((float)m[0], (float)m[1], (float)m[2]);
}

void computeSegmentation(Checker& a, bool fast) {
    if (a.base.empty()) return;
    int K = clamp(a.K, 2, 10);
    double s = min(1.0, (fast ? 96.0 : 160.0) / max(a.base.cols, a.base.rows));
    Mat sm, f32, lab;
    resize(a.base, sm, Size(), s, s, INTER_AREA);
    sm.convertTo(f32, CV_32FC3, 1.0 / 255.0);
    cvtColor(f32, lab, COLOR_BGR2Lab);
    Mat samples = lab.reshape(1, (int)lab.total());
    Mat labels, centers;
    kmeans(samples, K, labels, TermCriteria(TermCriteria::EPS + TermCriteria::COUNT, fast ? 10 : 20, 0.5),
           fast ? 1 : 3, KMEANS_PP_CENTERS, centers);

    vector<int> counts(K, 0);
    for (int i = 0; i < labels.rows; i++) counts[labels.at<int>(i)]++;

    vector<Cluster> raw(K);
    for (int k = 0; k < K; k++) {
        Mat c(1, 1, CV_32FC3, Scalar(centers.at<float>(k, 0), centers.at<float>(k, 1), centers.at<float>(k, 2)));
        Mat bgrM;
        cvtColor(c, bgrM, COLOR_Lab2BGR);
        Vec3f v = bgrM.at<Vec3f>(0, 0) * 255.f;
        for (int i = 0; i < 3; i++) v[i] = clamp(v[i], 0.f, 255.f);
        raw[k] = {v, 100.f * counts[k] / (float)labels.rows, toHex(v), colorName(v)};
    }

    Mat lm(sm.rows, sm.cols, CV_8U);
    for (int i = 0; i < labels.rows; i++) lm.data[i] = (uchar)labels.at<int>(i);
    Mat full;
    resize(lm, full, a.base.size(), 0, 0, INTER_NEAREST);

    a.labels.clear();
    double minArea = 0.012 * a.base.total();
    for (int k = 0; k < K; k++) {
        Mat mask, cc, stats, cents;
        compare(full, k, mask, CMP_EQ);
        int n = connectedComponentsWithStats(mask, cc, stats, cents, 8, CV_32S);
        vector<pair<int, int>> comps;
        for (int i = 1; i < n; i++)
            if (stats.at<int>(i, CC_STAT_AREA) >= minArea) comps.push_back({stats.at<int>(i, CC_STAT_AREA), i});
        sort(comps.rbegin(), comps.rend());
        bool light = toLab(raw[k].bgr)[0] > 55;
        for (size_t j = 0; j < comps.size() && j < 2; j++) {
            Point c(cvRound(cents.at<double>(comps[j].second, 0)), cvRound(cents.at<double>(comps[j].second, 1)));
            a.labels.push_back({c, raw[k].name, light ? Scalar(0, 0, 0) : Scalar(255, 255, 255),
                                light ? Scalar(255, 255, 255) : Scalar(0, 0, 0)});
        }
    }

    a.pal = raw;
    sort(a.pal.begin(), a.pal.end(), [](const Cluster& x, const Cluster& y) { return x.pct > y.pct; });

    a.warnings.clear();
    for (size_t i = 0; i < a.pal.size(); i++)
        for (size_t j = i + 1; j < a.pal.size(); j++) {
            double dOrig = norm(toLab(a.pal[i].bgr) - toLab(a.pal[j].bgr));
            if (dOrig < 20) continue;
            for (int t = 0; t < 3; t++) {
                double dSim = norm(toLab(simulatePixel(a.pal[i].bgr, t)) - toLab(simulatePixel(a.pal[j].bgr, t)));
                if (dSim < 10 && a.warnings.size() < 8)
                    a.warnings.push_back(a.pal[i].name + " (" + a.pal[i].hex + ") and " + a.pal[j].name + " (" +
                                         a.pal[j].hex + ") can look alike with " + string(DEF_NAMES[t]) + ".");
            }
        }
}

void updateView(Checker& a) {
    if (a.base.empty()) return;
    if (a.viewMode == 0) a.view = a.base;
    else if (a.viewMode == 1) a.view = simulateImage(a.base, a.defType);
    else a.view = daltonize(a.base, a.defType);
}

void setBase(Checker& a, const Mat& src, int maxW, int maxH) {
    double s = min(1.0, min((double)maxW / src.cols, (double)maxH / src.rows));
    if (s < 1.0) resize(src, a.base, Size(), s, s, INTER_AREA);
    else a.base = src.clone();
}

bool loadImage(Checker& a, const string& path) {
    Mat img = imread(path);
    if (img.empty()) return false;
    a.cap.release();
    a.live = a.frozen = false;
    setBase(a, img, 1000, 700);
    size_t sl = path.find_last_of("/\\");
    a.source = sl == string::npos ? path : path.substr(sl + 1);
    a.picked = false;
    computeSegmentation(a, false);
    updateView(a);
    return true;
}

bool startCam(Checker& a, int idx) {
    VideoCapture cap;
    if (!cap.open(idx)) return false;
    Mat f;
    for (int i = 0; i < 5; i++) cap.read(f);   // let auto-exposure settle
    if (f.empty()) return false;
    a.cap = cap;
    a.live = true; a.frozen = false; a.camIdx = idx; a.frameNo = 0;
    a.source = "Webcam " + to_string(idx);
    setBase(a, f, 960, 720);
    a.picked = true;
    a.pick = Point(a.base.cols / 2, a.base.rows / 2);
    a.pickBgr = avgColor(a.base, a.pick);
    computeSegmentation(a, true);
    updateView(a);
    return true;
}

void liveStep(Checker& a) {
    if (!a.live || a.frozen) return;
    Mat f;
    if (!a.cap.read(f) || f.empty()) { a.frozen = true; notify("Lost the camera feed."); return; }
    setBase(a, f, 960, 720);
    if (a.frameNo++ % 10 == 0) computeSegmentation(a, true);
    updateView(a);
    if (a.picked) a.pickBgr = avgColor(a.base, a.pick);
}

void leaveChecker(Checker& a) {
    if (a.live) { a.cap.release(); a.live = a.frozen = false; }
    a.typing = false;
}

// ============================================================ native file dialog
string pickFile() {
#ifdef _WIN32
    char file[MAX_PATH] = "";
    OPENFILENAMEA ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "Images\0*.jpg;*.jpeg;*.png;*.bmp;*.webp;*.tif;*.tiff\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = "Choose an image";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&ofn) ? string(file) : string();
#else
#ifdef __APPLE__
    string cmd = "osascript -e 'POSIX path of (choose file with prompt \"Choose an image\" of type {\"public.image\"})' 2>/dev/null";
#else
    string cmd = "zenity --file-selection --title='Choose an image' --file-filter='Images | *.jpg *.jpeg *.png *.bmp *.webp *.tif *.tiff' 2>/dev/null"
                 " || kdialog --getopenfilename . 'Images (*.jpg *.jpeg *.png *.bmp *.webp *.tif *.tiff)' 2>/dev/null";
#endif
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return "";
    char buf[2048];
    string out;
    while (fgets(buf, sizeof(buf), p)) out += buf;
    pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
#endif
}

void openImageDialog(Checker& a) {
    string p = pickFile();
    UI.down = false; UI.click = false;   // the dialog swallowed the mouse-up event
    if (p.empty()) { notify("No file chosen. (On Linux, install 'zenity' for the file dialog.)"); return; }
    if (!loadImage(a, p)) notify("Could not read that file as an image.");
}

// ============================================================ Ollama (free local AI)
static atomic<bool> g_busy{false};
static atomic<int> g_version{0};
static mutex g_mtx;
static string g_ai = "Press Describe to let the AI explain this image and its colors, or Ask... to type your own question.";
static string g_status;

static size_t writeCb(char* p, size_t sz, size_t n, void* ud) {
    static_cast<string*>(ud)->append(p, sz * n);
    return sz * n;
}
string base64(const vector<uchar>& d) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    string o;
    size_t i = 0;
    for (; i + 2 < d.size(); i += 3) {
        unsigned v = (d[i] << 16) | (d[i + 1] << 8) | d[i + 2];
        o += T[v >> 18]; o += T[(v >> 12) & 63]; o += T[(v >> 6) & 63]; o += T[v & 63];
    }
    if (i + 1 == d.size()) {
        unsigned v = d[i] << 16;
        o += T[v >> 18]; o += T[(v >> 12) & 63]; o += "==";
    } else if (i + 2 == d.size()) {
        unsigned v = (d[i] << 16) | (d[i + 1] << 8);
        o += T[v >> 18]; o += T[(v >> 12) & 63]; o += T[(v >> 6) & 63]; o += '=';
    }
    return o;
}
string ollamaGenerate(const string& model, const string& prompt, const Mat& img, string& err) {
    vector<uchar> buf;
    imencode(".jpg", img, buf, {IMWRITE_JPEG_QUALITY, 90});
    json req = {{"model", model}, {"prompt", prompt}, {"images", json::array({base64(buf)})},
                {"stream", false}, {"options", {{"temperature", 0.2}}}};
    string body = req.dump(), resp;
    CURL* c = curl_easy_init();
    if (!c) { err = "Could not start the HTTP client."; return ""; }
    curl_slist* h = curl_slist_append(nullptr, "Content-Type: application/json");
    curl_easy_setopt(c, CURLOPT_URL, "http://localhost:11434/api/generate");
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 600L);
    CURLcode rc = curl_easy_perform(c);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) {
        err = "Could not reach Ollama at localhost:11434. Install it from ollama.com, make sure it is running, "
              "and run 'ollama pull " + model + "'.";
        return "";
    }
    json j = json::parse(resp, nullptr, false);
    if (j.is_discarded()) { err = "Unexpected reply from Ollama."; return ""; }
    if (j.contains("error")) { err = j["error"].dump(); return ""; }
    return j.value("response", string());
}
void runAI(string model, string prompt, Mat img) {
    { lock_guard<mutex> l(g_mtx); g_status = "Asking " + model + "... (can take a while on a CPU)"; }
    string err;
    string r = ollamaGenerate(model, prompt, img, err);
    {
        lock_guard<mutex> l(g_mtx);
        g_ai = r.empty() ? "Error: " + err : r;
        g_status = r.empty() ? "Failed." : "Done.";
    }
    g_version++;
    g_busy = false;
}
string factsText(const Checker& a) {
    ostringstream o;
    o << "Facts measured by software (accurate; trust them over your own guess). Dominant colors: ";
    for (auto& c : a.pal) o << c.hex << " (" << c.name << ", " << cvRound(c.pct) << "%) ";
    if (a.picked)
        o << ". The user pointed at the spot " << cvRound(100.0 * a.pick.x / a.base.cols) << "% from the left and "
          << cvRound(100.0 * a.pick.y / a.base.rows) << "% from the top; its exact color is " << toHex(a.pickBgr)
          << " (" << lightnessWord(a.pickBgr) << " " << colorName(a.pickBgr) << ")";
    o << ".";
    return o.str();
}
static const char* PREAMBLE =
    "You are a helpful assistant for people with color vision deficiency (color blindness). "
    "Use plain, simple language. Never invent text or details you cannot see; say when you are unsure. ";

void startAI(Checker& a, const string& model, char mode, const string& question) {
    if (g_busy) { notify("The AI is still working..."); return; }
    if (a.base.empty()) { notify("Open an image or start the webcam first."); return; }
    string prompt;
    Mat send = a.base;
    if (mode == 'c') {
        if (!a.picked) { notify("Click a spot on the image first."); return; }
        int half = max(40, (int)(min(a.base.cols, a.base.rows) * 0.15));
        Rect r(a.pick.x - half, a.pick.y - half, 2 * half, 2 * half);
        r &= Rect(0, 0, a.base.cols, a.base.rows);
        send = a.base(r).clone();
        if (min(send.cols, send.rows) < 200) resize(send, send, Size(), 2, 2, INTER_CUBIC);
        prompt = string(PREAMBLE) +
                 "This image is a close-up of the spot the user pointed at. Say what the object is and what color it is "
                 "in plain words (light/dark, bright/dull, similar to a familiar thing). " + factsText(a);
    } else {
        prompt = string(PREAMBLE) +
                 "Look at the image and: 1) say briefly what it shows; 2) list the main objects and the plain-English "
                 "color of each; 3) point out places where easily confused colors (red/green, blue/purple, brown/green, "
                 "pink/gray) appear together and give non-color clues (position, shape, text, brightness) that help "
                 "tell them apart. Keep it short, in bullet points. " + factsText(a);
        if (!question.empty())
            prompt += "\nThe user asks: " + question + "\nAnswer that question first, then add anything useful for a color-blind person.";
    }
    g_busy = true;
    thread(runAI, model, prompt, send.clone()).detach();
}

void speak(const string& text) {
    thread([text] {
        { ofstream f("speak_tmp.txt"); f << text; }
#if defined(__APPLE__)
        (void)system("say -f speak_tmp.txt");
#elif defined(_WIN32)
        (void)system("powershell -NoProfile -Command \"Add-Type -AssemblyName System.Speech; (New-Object System.Speech.Synthesis.SpeechSynthesizer).Speak((Get-Content -Raw speak_tmp.txt))\"");
#else
        (void)system("espeak-ng -f speak_tmp.txt 2>/dev/null || espeak -f speak_tmp.txt 2>/dev/null");
#endif
    }).detach();
}

string paletteReport(const Checker& a, const string& ai) {
    ostringstream o;
    o << "COLOR REPORT (" << a.source << ")\n\nImage palette:\n";
    for (auto& c : a.pal) o << "  " << c.hex << "  " << c.name << "  " << cvRound(c.pct) << "%\n";
    if (a.picked)
        o << "\nPicked color: " << toHex(a.pickBgr) << "  " << colorName(a.pickBgr) << " (" << lightnessWord(a.pickBgr)
          << ")  " << toRGB(a.pickBgr) << "  " << toHSV(a.pickBgr) << "\n";
    if (!a.warnings.empty()) { o << "\nColors that may look alike:\n"; for (auto& w : a.warnings) o << "  - " << w << "\n"; }
    o << "\nAI assistant:\n" << ai << "\n";
    return o.str();
}
void exportReport(const Checker& a) {
    if (a.base.empty()) { notify("Nothing to export yet."); return; }
    string ai;
    { lock_guard<mutex> l(g_mtx); ai = g_ai; }
    ofstream("color_report.txt") << paletteReport(a, ai);
    imwrite("color_view.png", a.view);
    notify("Saved color_report.txt and color_view.png");
}
void startCamUI(Checker& a) {
    if (!startCam(a, a.camIdx)) notify("Could not open camera " + to_string(a.camIdx) + ". Try the Cam button.");
}

// ============================================================ checker screen
void drawChecker(Mat& c, Checker& a, const string& model, Screen& scr) {
    // ---------- top bar
    rectangle(c, Rect(0, 0, WW, TOP), C_PANEL, FILLED);
    if (button(c, Rect(12, 10, 90, 36), "< Home")) { leaveChecker(a); scr = S_MENU; return; }
    txt(c, "Color Checker", Point(118, 36), 0.8, C_TXT, 2);
    if (!a.source.empty()) txt(c, a.source.substr(0, 34), Point(330, 35), 0.45, C_DIM);
    if (button(c, Rect(WW - 142, 10, 130, 36), "Open file")) openImageDialog(a);
    if (button(c, Rect(WW - 252, 10, 100, 36), "Webcam", a.live)) startCamUI(a);
    if (a.live) {
        if (button(c, Rect(WW - 362, 10, 100, 36), a.frozen ? "Resume" : "Freeze", a.frozen)) a.frozen = !a.frozen;
        if (button(c, Rect(WW - 472, 10, 100, 36), "Cam " + to_string(a.camIdx))) {
            int old = a.camIdx, nx = (old + 1) % 4;
            if (!startCam(a, nx)) { notify("No camera found at index " + to_string(nx)); startCam(a, old); }
        }
    }

    // ---------- image area
    Rect area(0, TOP, PNL_X, WH - TOP - STRIP);
    if (a.view.empty()) {
        txtC(c, "Choose what to check", Rect(area.x, area.y + 150, area.width, 40), 0.9, C_TXT, 2);
        if (button(c, Rect(area.width / 2 - 210, area.y + 220, 200, 56), "Open an image", false, true, 0.65)) openImageDialog(a);
        if (button(c, Rect(area.width / 2 + 10, area.y + 220, 200, 56), "Use webcam", false, true, 0.65)) startCamUI(a);
    } else {
        double s = min((double)area.width / a.view.cols, (double)area.height / a.view.rows);
        int w = cvRound(a.view.cols * s), h = cvRound(a.view.rows * s);
        a.disp = Rect(area.x + (area.width - w) / 2, area.y + (area.height - h) / 2, w, h);
        a.dscale = (double)w / a.view.cols;
        Mat sc;
        resize(a.view, sc, Size(w, h), 0, 0, s < 1 ? INTER_AREA : INTER_LINEAR);
        if (a.showLabels)
            for (auto& l : a.labels) {
                Point q(cvRound(l.pt.x * a.dscale), cvRound(l.pt.y * a.dscale));
                Size ts = tsz(l.text, 0.5);
                Point org(q.x - ts.width / 2, q.y + ts.height / 2);
                putText(sc, l.text, org, FONT_HERSHEY_SIMPLEX, 0.5, l.outline, 3, LINE_AA);
                putText(sc, l.text, org, FONT_HERSHEY_SIMPLEX, 0.5, l.col, 1, LINE_AA);
            }
        if (a.picked) {
            Point q(cvRound(a.pick.x * a.dscale), cvRound(a.pick.y * a.dscale));
            circle(sc, q, 12, Scalar(0, 0, 0), 3, LINE_AA);
            circle(sc, q, 12, Scalar(255, 255, 255), 1, LINE_AA);
        }
        sc.copyTo(c(a.disp));
        // tags
        string tag = a.viewMode == 0 ? "Original" : string(VIEW_NAMES[a.viewMode]) + ": " + DEF_NAMES[a.defType];
        Size tz = tsz(tag, 0.5);
        fillRound(c, Rect(a.disp.x + 10, a.disp.y + 10, tz.width + 20, 28), 8, rgb(10, 10, 14));
        txt(c, tag, Point(a.disp.x + 20, a.disp.y + 29), 0.5, C_TXT);
        if (a.live) {
            string lt = a.frozen ? "FROZEN" : "LIVE";
            Size lz = tsz(lt, 0.5);
            fillRound(c, Rect(a.disp.x + a.disp.width - lz.width - 44, a.disp.y + 10, lz.width + 34, 28), 8, rgb(10, 10, 14));
            circle(c, Point(a.disp.x + a.disp.width - lz.width - 28, a.disp.y + 24), 5, a.frozen ? C_ACC2 : C_BAD, FILLED, LINE_AA);
            txt(c, lt, Point(a.disp.x + a.disp.width - lz.width - 16, a.disp.y + 29), 0.5, C_TXT);
        }
        // pick with mouse (click or drag)
        if (UI.down && a.disp.contains(Point(UI.mx, UI.my))) {
            a.pick = Point(clamp(cvRound((UI.mx - a.disp.x) / a.dscale), 0, a.base.cols - 1),
                           clamp(cvRound((UI.my - a.disp.y) / a.dscale), 0, a.base.rows - 1));
            a.pickBgr = avgColor(a.base, a.pick);
            a.picked = true;
        }
    }

    // ---------- control strip
    int sy = WH - STRIP;
    rectangle(c, Rect(0, sy, PNL_X, STRIP), C_PANEL, FILLED);
    txt(c, "View:", Point(14, sy + 32), 0.5, C_DIM);
    for (int i = 0; i < 3; i++)
        if (button(c, Rect(74 + i * 116, sy + 8, 108, 36), VIEW_NAMES[i], a.viewMode == i)) { a.viewMode = i; updateView(a); }
    if (button(c, Rect(430, sy + 8, 100, 36), "Labels", a.showLabels)) a.showLabels = !a.showLabels;
    txt(c, "Colors", Point(552, sy + 32), 0.5, C_DIM);
    if (button(c, Rect(616, sy + 8, 36, 36), "-", false, a.K > 2, 0.7)) { a.K--; computeSegmentation(a, a.live); }
    txtC(c, to_string(a.K), Rect(652, sy + 8, 36, 36), 0.6, C_TXT);
    if (button(c, Rect(688, sy + 8, 36, 36), "+", false, a.K < 10, 0.7)) { a.K++; computeSegmentation(a, a.live); }

    const char* dn[3] = {"Protan", "Deutan", "Tritan"};
    txt(c, "Type:", Point(14, sy + 84), 0.5, C_DIM);
    for (int i = 0; i < 3; i++)
        if (button(c, Rect(74 + i * 116, sy + 60, 108, 36), dn[i], a.defType == i && a.viewMode != 0)) { a.defType = i; updateView(a); }
    txt(c, DEF_NAMES[a.defType], Point(430, sy + 84), 0.45, C_DIM);
    if (button(c, Rect(698, sy + 60, 92, 36), "Export")) exportReport(a);

    // ---------- right panel
    rectangle(c, Rect(PNL_X, TOP, PNL_W, WH - TOP), C_PANEL, FILLED);
    int x = PNL_X + 16, y = TOP + 26;
    txt(c, "Picked color", Point(x, y), 0.6, C_ACC2);
    y += 12;
    if (a.picked && !a.base.empty()) {
        fillRound(c, Rect(x, y, 72, 72), 10, C_LINE);
        fillRound(c, Rect(x + 2, y + 2, 68, 68), 8, Scalar(a.pickBgr[0], a.pickBgr[1], a.pickBgr[2]));
        txt(c, toHex(a.pickBgr), Point(x + 88, y + 22), 0.7, C_TXT, 2);
        txt(c, lightnessWord(a.pickBgr) + " " + colorName(a.pickBgr), Point(x + 88, y + 44), 0.5, C_TXT);
        txt(c, toRGB(a.pickBgr), Point(x + 88, y + 62), 0.45, C_DIM);
        txt(c, toHSV(a.pickBgr), Point(x + 88, y + 80), 0.45, C_DIM);
        if (button(c, Rect(PNL_X + PNL_W - 86, y - 4, 70, 26), "Say it", false, true, 0.42))
            speak("This color looks like " + lightnessWord(a.pickBgr) + " " + colorName(a.pickBgr) + ".");
        y += 96;
    } else {
        y += 22;
        txt(c, "Click or drag on the image.", Point(x, y), 0.5, C_DIM);
        y += 22;
    }

    txt(c, "Image palette", Point(x, y), 0.6, C_ACC2);
    y += 10;
    for (auto& p : a.pal) {
        fillRound(c, Rect(x, y, 28, 18), 4, C_LINE);
        fillRound(c, Rect(x + 1, y + 1, 26, 16), 3, Scalar(p.bgr[0], p.bgr[1], p.bgr[2]));
        txt(c, p.hex, Point(x + 38, y + 14), 0.45, C_TXT);
        txt(c, p.name, Point(x + 112, y + 14), 0.45, C_TXT);
        int bw = cvRound(p.pct / 100.f * 80.f);
        rectangle(c, Rect(PNL_X + PNL_W - 16 - 80 - 40, y + 5, max(bw, 1), 8), C_ACC, FILLED);
        char b[16];
        snprintf(b, sizeof(b), "%2.0f%%", p.pct);
        txt(c, b, Point(PNL_X + PNL_W - 16 - 34, y + 14), 0.45, C_DIM);
        y += 22;
    }

    if (!a.warnings.empty()) {
        y += 12;
        txt(c, "May look alike to colorblind people", Point(x, y), 0.55, C_BAD);
        int shown = 0;
        for (size_t i = 0; i < a.warnings.size() && shown < 5; i++)
            for (auto& ln : wrapText(a.warnings[i], PNL_W - 36, 0.4)) {
                if (shown >= 5) break;
                y += 15; txt(c, ln, Point(x + 2, y), 0.4, C_DIM); shown++;
            }
        y += 4;
    }

    y += 26;
    txt(c, "AI assistant", Point(x, y), 0.6, C_ACC2);
    string status;
    { lock_guard<mutex> l(g_mtx); status = g_status; }
    if (!status.empty()) txt(c, status.substr(0, 30), Point(x + 130, y), 0.4, rgb(90, 220, 240));
    y += 10;
    int bw = (PNL_W - 32 - 18) / 4;
    if (button(c, Rect(x, y, bw, 30), "Describe", false, !g_busy, 0.42)) startAI(a, model, 'a', "");
    if (button(c, Rect(x + (bw + 6), y, bw, 30), "Ask...", a.typing, !g_busy, 0.42)) { a.typing = true; a.typed.clear(); }
    if (button(c, Rect(x + 2 * (bw + 6), y, bw, 30), "Spot", false, !g_busy && a.picked, 0.42)) startAI(a, model, 'c', "");
    if (button(c, Rect(x + 3 * (bw + 6), y, bw, 30), "Read aloud", false, true, 0.4)) {
        string t;
        { lock_guard<mutex> l(g_mtx); t = g_ai; }
        speak(asciiOnly(t));
    }
    y += 40;
    if (a.typing) {
        fillRound(c, Rect(x, y, PNL_W - 32, 30), 8, C_ACC);
        fillRound(c, Rect(x + 1, y + 1, PNL_W - 34, 28), 7, C_BG);
        string shown = a.typed;
        while (shown.size() > 1 && tsz("Ask: " + shown + "_", 0.5).width > PNL_W - 52) shown.erase(0, 1);
        txt(c, "Ask: " + shown + "_", Point(x + 10, y + 20), 0.5, C_TXT);
        y += 34;
        txt(c, "Enter = send (empty = general description), Esc = cancel", Point(x, y + 8), 0.38, C_DIM);
        y += 18;
    }

    int ver = g_version.load();
    if (a.aiVer != ver) {
        string t;
        { lock_guard<mutex> l(g_mtx); t = g_ai; }
        a.aiBlock = renderBlock(asciiOnly(t), PNL_W - 40, 0.48, C_TXT);
        a.aiVer = ver;
        a.aiScroll = 0;
    }
    int availH = WH - 8 - y;
    if (availH > 20) {
        a.aiMax = max(0, a.aiBlock.rows - availH);
        if (hot(Rect(PNL_X, TOP, PNL_W, WH - TOP))) a.aiScroll -= UI.wheel / 3;
        a.aiScroll = clamp(a.aiScroll, 0, a.aiMax);
        int h = min(availH, a.aiBlock.rows - a.aiScroll);
        a.aiBlock(Rect(0, a.aiScroll, a.aiBlock.cols, h)).copyTo(c(Rect(x, y, a.aiBlock.cols, h)));
        if (a.aiMax > 0) {
            int th = max(24, availH * availH / a.aiBlock.rows);
            int ty = y + (availH - th) * a.aiScroll / a.aiMax;
            rectangle(c, Rect(PNL_X + PNL_W - 8, ty, 4, th), C_LINE, FILLED);
        }
    }
}

void checkerKey(Checker& a, int key, const string& model) {
    if (a.typing) {
        if (key == 27) a.typing = false;
        else if (key == 13 || key == 10) { a.typing = false; startAI(a, model, 'a', a.typed); a.typed.clear(); }
        else if (key == 8 || key == 127) { if (!a.typed.empty()) a.typed.pop_back(); }
        else if (key >= 32 && key < 127 && a.typed.size() < 200) a.typed += (char)key;
        return;
    }
    switch (key) {
        case '1': case '2': case '3': a.viewMode = key - '1'; updateView(a); break;
        case 'd': a.defType = (a.defType + 1) % 3; updateView(a); break;
        case 'l': a.showLabels = !a.showLabels; break;
        case ' ': if (a.live) a.frozen = !a.frozen; break;
        case 'o': openImageDialog(a); break;
        case 'w': startCamUI(a); break;
        case 'a': startAI(a, model, 'a', ""); break;
        case 't': a.typing = true; a.typed.clear(); break;
        case 'c': startAI(a, model, 'c', ""); break;
        case 'v': if (a.picked) speak("This color looks like " + lightnessWord(a.pickBgr) + " " + colorName(a.pickBgr) + "."); break;
        case 'n': { string t; { lock_guard<mutex> l(g_mtx); t = g_ai; } speak(asciiOnly(t)); break; }
        case 'e': exportReport(a); break;
        case 'j': a.aiScroll = min(a.aiScroll + 40, a.aiMax); break;
        case 'k': a.aiScroll = max(0, a.aiScroll - 40); break;
    }
}

// ============================================================ Ishihara-style plates
enum { P_CONTROL, P_RGA, P_RGB, P_BYA, P_BYB };

Mat makePlate(int number, int type, int S, unsigned seed) {
    mt19937 rng(seed);
    auto U = [&](float lo, float hi) { return uniform_real_distribution<float>(lo, hi)(rng); };
    vector<Scalar> fig, bg;
    switch (type) {
        case P_CONTROL:   // strong lightness contrast: everyone should see it
            fig = {rgb(30, 50, 110), rgb(50, 70, 130), rgb(20, 40, 95)};
            bg = {rgb(235, 210, 160), rgb(225, 195, 140), rgb(240, 220, 175)}; break;
        case P_RGA:       // orange/red figure on green
            fig = {rgb(232, 124, 66), rgb(240, 150, 88), rgb(220, 98, 58), rgb(245, 170, 105)};
            bg = {rgb(150, 172, 84), rgb(170, 186, 104), rgb(124, 152, 72), rgb(188, 196, 120), rgb(140, 160, 90)}; break;
        case P_RGB:       // green figure on orange/red
            bg = {rgb(232, 124, 66), rgb(240, 150, 88), rgb(220, 98, 58), rgb(245, 170, 105)};
            fig = {rgb(150, 172, 84), rgb(170, 186, 104), rgb(124, 152, 72), rgb(188, 196, 120), rgb(140, 160, 90)}; break;
        case P_BYA:       // blue figure on green
            fig = {rgb(70, 140, 185), rgb(90, 160, 200), rgb(55, 125, 170)};
            bg = {rgb(95, 170, 120), rgb(120, 185, 135), rgb(80, 155, 105), rgb(140, 190, 140)}; break;
        default:          // yellow figure on pink/lilac
            fig = {rgb(235, 205, 95), rgb(240, 215, 120), rgb(225, 190, 80)};
            bg = {rgb(205, 165, 190), rgb(190, 150, 180), rgb(215, 180, 200), rgb(200, 160, 170)}; break;
    }
    // digit mask
    Mat mask(S, S, CV_8U, Scalar(0));
    string t = to_string(number);
    int th = 1, bl;
    double sc = 10;
    Size ts;
    for (; sc > 0.5; sc -= 0.25) {
        th = max(1, cvRound(sc * 3.2));
        ts = getTextSize(t, FONT_HERSHEY_DUPLEX, sc, th, &bl);
        if (ts.width < S * 0.56 && ts.height < S * 0.42) break;
    }
    putText(mask, t, Point((S - ts.width) / 2, (S + ts.height) / 2), FONT_HERSHEY_DUPLEX, sc, Scalar(255), th, LINE_AA);

    // pack dots (grid-accelerated)
    struct D { float x, y, r; };
    vector<D> dots;
    const float rMax = S / 60.f, rMin = S / 180.f, gap = max(1.f, S / 360.f);
    const float cell = 2 * rMax + gap + 1;
    const int gw = (int)(S / cell) + 2;
    vector<vector<int>> grid(gw * gw);
    const float R = S / 2.f - 3, cx = S / 2.f, cy = S / 2.f;
    for (float r = rMax; r >= rMin; r -= rMax / 14.f) {
        int tries = r > rMax * 0.6f ? 3000 : 7000;
        for (int i = 0; i < tries; i++) {
            float ang = U(0, 6.2831853f), rad = sqrt(U(0, 1)) * (R - r);
            float x = cx + rad * cos(ang), y = cy + rad * sin(ang);
            int gx = (int)(x / cell), gy = (int)(y / cell);
            bool ok = true;
            for (int dy = -1; dy <= 1 && ok; dy++)
                for (int dx = -1; dx <= 1 && ok; dx++) {
                    int nx = gx + dx, ny = gy + dy;
                    if (nx < 0 || ny < 0 || nx >= gw || ny >= gw) continue;
                    for (int id : grid[ny * gw + nx]) {
                        const D& o = dots[id];
                        float ddx = o.x - x, ddy = o.y - y, mn = o.r + r + gap;
                        if (ddx * ddx + ddy * ddy < mn * mn) { ok = false; break; }
                    }
                }
            if (!ok) continue;
            grid[gy * gw + gx].push_back((int)dots.size());
            dots.push_back({x, y, r});
        }
    }
    Mat plate(S, S, CV_8UC3, C_BG);
    const int ox[5] = {0, 1, -1, 0, 0}, oy[5] = {0, 0, 0, 1, -1};
    for (auto& d : dots) {
        int inside = 0;
        for (int k = 0; k < 5; k++) {
            int px = clamp(cvRound(d.x + ox[k] * d.r * 0.5f), 0, S - 1), py = clamp(cvRound(d.y + oy[k] * d.r * 0.5f), 0, S - 1);
            inside += mask.at<uchar>(py, px) > 0;
        }
        const vector<Scalar>& pal = inside >= 3 ? fig : bg;
        Scalar col = pal[rng() % pal.size()];
        float f = U(0.88f, 1.1f);
        col = Scalar(min(255.0, col[0] * f), min(255.0, col[1] * f), min(255.0, col[2] * f));
        circle(plate, Point(cvRound(d.x), cvRound(d.y)), max(1, cvRound(d.r)), col, FILLED, LINE_AA);
    }
    return plate;
}

// ============================================================ colorblind test screen
struct PlateDef { int num, type; unsigned seed; };
struct TestState {
    int stage = 0;   // 0 intro, 1 running, 2 results
    vector<PlateDef> plates;
    vector<Mat> imgs;
    vector<int> answers;   // -1 = could not see a number
    int idx = 0;
    string input;
};

void buildPlates(TestState& t) {
    random_device rd;
    mt19937 g(rd());
    const int types[] = {P_CONTROL, P_RGA, P_RGB, P_RGA, P_BYA, P_RGB, P_BYB, P_RGA};
    set<int> used;
    t.plates.clear(); t.answers.clear();
    for (int ty : types) {
        int n;
        do { n = (g() % 4 == 0) ? 2 + (int)(g() % 8) : 10 + (int)(g() % 89); } while (used.count(n));
        used.insert(n);
        t.plates.push_back({n, ty, (unsigned)g()});
    }
    t.imgs.assign(t.plates.size(), Mat());
    t.idx = 0; t.input.clear(); t.stage = 1;
}
void submitAnswer(TestState& t, int ans) {
    t.answers.push_back(ans);
    t.input.clear();
    if (++t.idx >= (int)t.plates.size()) t.stage = 2;
}
void trySubmit(TestState& t) {
    if (t.input.empty()) { notify("Type the number you see, or press \"I see no number\"."); return; }
    submitAnswer(t, atoi(t.input.c_str()));
}

void drawTest(Mat& c, TestState& t, Screen& scr) {
    rectangle(c, Rect(0, 0, WW, TOP), C_PANEL, FILLED);
    if (button(c, Rect(12, 10, 90, 36), "< Home")) { scr = S_MENU; return; }
    txt(c, "Colorblind Test", Point(118, 36), 0.8, C_TXT, 2);

    if (t.stage == 0) {
        txtC(c, "Quick color vision screening", Rect(0, 90, WW, 50), 1.0, C_TXT, 2);
        vector<string> tips = {
            "Before you start:",
            "- Set your screen brightness to a normal level and turn off night mode / color filters.",
            "- Sit about an arm's length from the screen in normal room light.",
            "- Wear your glasses or contacts if you normally use them.",
            "",
            "You will see 8 dot patterns. Type the number you see (or use the keypad).",
            "If you cannot see any number, press \"I see no number\".",
            "",
            "This is a screening only - it is NOT a medical diagnosis."};
        int y = 190;
        for (auto& tp : tips) {
            for (auto& ln : wrapText(tp, 900, 0.65)) { txt(c, ln, Point(150, y), 0.65, tp[0] == 'B' ? C_ACC2 : C_TXT); y += 34; }
            if (tp.empty()) y -= 10;
        }
        if (button(c, Rect(WW / 2 - 110, 560, 220, 60), "Start test", true, true, 0.8)) buildPlates(t);
    } else if (t.stage == 1) {
        const int S = 540;
        if (t.imgs[t.idx].empty()) t.imgs[t.idx] = makePlate(t.plates[t.idx].num, t.plates[t.idx].type, S, t.plates[t.idx].seed);
        t.imgs[t.idx].copyTo(c(Rect(60, 110, S, S)));
        int px = 660;
        txt(c, "Plate " + to_string(t.idx + 1) + " of " + to_string(t.plates.size()), Point(px, 130), 0.85, C_TXT, 2);
        rectangle(c, Rect(px, 148, 480, 8), C_CARD, FILLED);
        rectangle(c, Rect(px, 148, 480 * t.idx / (int)t.plates.size(), 8), C_ACC, FILLED);
        txt(c, "What number do you see?", Point(px, 196), 0.7, C_DIM);
        fillRound(c, Rect(px, 214, 480, 76), 12, C_LINE);
        fillRound(c, Rect(px + 2, 216, 476, 72), 10, C_CARD);
        txt(c, t.input + "_", Point(px + 20, 270), 1.8, C_TXT, 3);
        // keypad
        const char* keys[12] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "Clear", "0", "Del"};
        for (int i = 0; i < 12; i++) {
            Rect r(px + (i % 3) * 110, 312 + (i / 3) * 68, 100, 58);
            if (button(c, r, keys[i], false, true, i == 9 || i == 11 ? 0.6 : 0.9)) {
                if (i == 9) t.input.clear();
                else if (i == 11) { if (!t.input.empty()) t.input.pop_back(); }
                else if (t.input.size() < 2) t.input += keys[i];
            }
        }
        if (button(c, Rect(px + 340, 312, 140, 58), "Submit", true, true, 0.7)) trySubmit(t);
        if (button(c, Rect(px + 340, 380, 140, 58), "I see no number", false, true, 0.42)) submitAnswer(t, -1);
        txt(c, "Keyboard: digits, Enter = submit, Backspace = delete", Point(px, 612), 0.45, C_DIM);
    } else {
        int ctrl = 0, rg = 0, by = 0, rgN = 0, byN = 0;
        for (size_t i = 0; i < t.plates.size(); i++) {
            bool ok = t.answers[i] == t.plates[i].num;
            if (t.plates[i].type == P_CONTROL) ctrl = ok;
            else if (t.plates[i].type == P_RGA || t.plates[i].type == P_RGB) { rgN++; rg += ok; }
            else { byN++; by += ok; }
        }
        txt(c, "Your results", Point(60, 100), 1.0, C_TXT, 2);
        int y = 150;
        if (!ctrl) {
            for (auto& ln : wrapText("Inconclusive: the control plate (which everyone should see) was missed. Check your screen settings, lighting and glasses, then retake the test.", 1080, 0.6)) {
                txt(c, ln, Point(60, y), 0.6, C_ACC2); y += 28;
            }
            y += 6;
        }
        string rgV = rg >= rgN - 1 ? "Normal" : rg >= rgN - 2 ? "Possible mild red-green difficulty" : "Likely red-green color vision deficiency";
        Scalar rgC = rg >= rgN - 1 ? C_GOOD : rg >= rgN - 2 ? C_ACC2 : C_BAD;
        string byV = by == byN ? "Normal" : by == 1 ? "Borderline" : "Possible blue-yellow difficulty";
        Scalar byC = by == byN ? C_GOOD : by == 1 ? C_ACC2 : C_BAD;
        txt(c, "Red-green plates:  " + to_string(rg) + " / " + to_string(rgN), Point(60, y + 10), 0.7, C_TXT);
        txt(c, rgV, Point(400, y + 10), 0.7, rgC, 2);
        txt(c, "Blue-yellow plates:  " + to_string(by) + " / " + to_string(byN), Point(60, y + 46), 0.7, C_TXT);
        txt(c, byV, Point(400, y + 46), 0.7, byC, 2);
        y += 90;
        for (auto& ln : wrapText("Tip: open the Color Checker and use the Simulated / Enhanced views to see which colors are hard to tell apart and to get help distinguishing them. "
                                 "This quick test depends on your screen and is not a medical diagnosis - please see an eye-care professional for a proper exam.", 1080, 0.5)) {
            txt(c, ln, Point(60, y), 0.5, C_DIM); y += 22;
        }
        // thumbnails
        int ty = max(y + 20, 340);
        for (size_t i = 0; i < t.plates.size(); i++) {
            int x = 60 + (int)i * 138;
            Mat th;
            resize(t.imgs[i], th, Size(120, 120), 0, 0, INTER_AREA);
            th.copyTo(c(Rect(x, ty, 120, 120)));
            bool ok = t.answers[i] == t.plates[i].num;
            txt(c, "Answer: " + to_string(t.plates[i].num), Point(x, ty + 142), 0.45, C_DIM);
            txt(c, t.answers[i] < 0 ? "You: none" : "You: " + to_string(t.answers[i]), Point(x, ty + 162), 0.45, ok ? C_GOOD : C_BAD);
        }
        if (button(c, Rect(60, WH - 80, 160, 48), "Retake", true, true, 0.65)) t = TestState();
        if (button(c, Rect(240, WH - 80, 160, 48), "Home", false, true, 0.65)) scr = S_MENU;
    }
}
void testKey(TestState& t, int key) {
    if (t.stage != 1) return;
    if (key >= '0' && key <= '9' && t.input.size() < 2) t.input += (char)key;
    else if (key == 8 || key == 127) { if (!t.input.empty()) t.input.pop_back(); }
    else if (key == 13 || key == 10) trySubmit(t);
}

// ============================================================ menu
bool card(Mat& c, Rect r, const string& title, const string& l1, const string& l2, int icon) {
    bool h = hot(r);
    fillRound(c, r, 18, h ? C_ACC : C_LINE);
    fillRound(c, Rect(r.x + 2, r.y + 2, r.width - 4, r.height - 4), 16, h ? C_HOT : C_CARD);
    int cx = r.x + r.width / 2, cy = r.y + 95;
    if (icon == 0) {
        Scalar cols[6] = {rgb(230, 60, 60), rgb(245, 160, 40), rgb(240, 220, 50), rgb(60, 180, 90), rgb(60, 130, 240), rgb(160, 90, 220)};
        for (int i = 0; i < 6; i++) circle(c, Point(cx - 48 + (i % 3) * 48, cy - 24 + (i / 3) * 48), 20, cols[i], FILLED, LINE_AA);
    } else if (icon == 1) {
        fillRound(c, Rect(cx - 22, cy - 46, 44, 18), 5, rgb(200, 205, 215));
        fillRound(c, Rect(cx - 58, cy - 34, 116, 80), 12, rgb(200, 205, 215));
        circle(c, Point(cx, cy + 6), 26, rgb(40, 44, 56), FILLED, LINE_AA);
        circle(c, Point(cx, cy + 6), 16, C_ACC, FILLED, LINE_AA);
        circle(c, Point(cx + 40, cy - 20), 5, C_BAD, FILLED, LINE_AA);
    } else {
        static Mat ic = makePlate(5, P_RGA, 130, 7);
        static Mat cm = [] { Mat m(130, 130, CV_8U, Scalar(0)); circle(m, Point(65, 65), 64, Scalar(255), FILLED, LINE_AA); return m; }();
        Mat roi = c(Rect(cx - 65, cy - 65, 130, 130));
        ic.copyTo(roi, cm);
    }
    txtC(c, title, Rect(r.x, r.y + 190, r.width, 36), 0.85, C_TXT, 2);
    txtC(c, l1, Rect(r.x, r.y + 232, r.width, 24), 0.5, C_DIM);
    txtC(c, l2, Rect(r.x, r.y + 258, r.width, 24), 0.5, C_DIM);
    if (h && UI.click) { UI.click = false; return true; }
    return false;
}
int drawMenu(Mat& c) {
    int res = 0;
    txtC(c, "COLOR ASSISTANT", Rect(0, 50, WW, 70), 1.7, C_TXT, 3);
    txtC(c, "See colors clearly - check any image or camera view, or test your own color vision.", Rect(0, 118, WW, 30), 0.62, C_DIM);
    const int cw = 340, ch = 320, gap = 30, x0 = (WW - 3 * cw - 2 * gap) / 2, y0 = 200;
    if (card(c, Rect(x0, y0, cw, ch), "Check an image", "Pick a photo from your computer", "Exact color codes + AI help", 0)) res = 1;
    if (card(c, Rect(x0 + cw + gap, y0, cw, ch), "Use the webcam", "Live color picking and", "colorblind simulation", 1)) res = 2;
    if (card(c, Rect(x0 + 2 * (cw + gap), y0, cw, ch), "Colorblind test", "8 dot plates, takes 2 minutes", "Red-green and blue-yellow", 2)) res = 3;
    txtC(c, "Click a card to begin.  Esc = back, Q = quit", Rect(0, WH - 70, WW, 30), 0.5, C_DIM);
    if (button(c, Rect(WW - 130, WH - 56, 100, 36), "Quit")) res = 4;
    return res;
}

void drawToast(Mat& c) {
    if (g_noticeTtl <= 0) return;
    g_noticeTtl--;
    Size z = tsz(g_notice, 0.55);
    Rect r((WW - z.width) / 2 - 18, TOP + 12, z.width + 36, 38);
    fillRound(c, r, 10, C_ACC2);
    fillRound(c, Rect(r.x + 2, r.y + 2, r.width - 4, r.height - 4), 9, rgb(10, 10, 14));
    txtC(c, g_notice, r, 0.55, C_TXT);
}

// ============================================================ main
int main(int argc, char** argv) {
    string model = "llava", startImg;
    for (int i = 1; i < argc; i++) {
        string s = argv[i];
        if (ifstream(s).good()) startImg = s;
        else model = s;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    const string WIN = "Color Assistant";
    namedWindow(WIN, WINDOW_AUTOSIZE);
    setMouseCallback(WIN, onMouse);

    Mat canvas(WH, WW, CV_8UC3);
    Screen scr = S_MENU;
    Checker chk;
    TestState test;
    if (!startImg.empty()) {
        if (loadImage(chk, startImg)) scr = S_CHECK;
        else notify("Could not read " + startImg);
    }

    bool quit = false;
    while (!quit) {
        canvas.setTo(C_BG);
        if (scr == S_MENU) {
            int ch = drawMenu(canvas);
            if (ch == 1) { scr = S_CHECK; if (chk.view.empty() || chk.live) openImageDialog(chk); }
            else if (ch == 2) { scr = S_CHECK; startCamUI(chk); }
            else if (ch == 3) { test = TestState(); scr = S_TEST; }
            else if (ch == 4) quit = true;
        } else if (scr == S_CHECK) {
            liveStep(chk);
            drawChecker(canvas, chk, model, scr);
        } else {
            drawTest(canvas, test, scr);
        }
        drawToast(canvas);
        UI.click = false;
        UI.wheel = 0;
        imshow(WIN, canvas);
        int key = waitKey(15);
        if (getWindowProperty(WIN, WND_PROP_VISIBLE) < 1) break;
        if (key != -1) {
            key &= 0xFF;
            if (scr == S_CHECK && chk.typing) checkerKey(chk, key, model);
            else if (key == 'q') quit = true;
            else if (key == 27) {
                if (scr == S_MENU) quit = true;
                else { if (scr == S_CHECK) leaveChecker(chk); scr = S_MENU; }
            }
            else if (scr == S_CHECK) checkerKey(chk, key, model);
            else if (scr == S_TEST) testKey(test, key);
        }
    }
    if (g_busy) _Exit(0);   // an AI request is still running; don't wait for it
    return 0;
}
