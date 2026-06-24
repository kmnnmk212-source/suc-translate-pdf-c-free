/*
 * PDF Translator to Arabic v3 - إصلاح علق CUDA Graph
 */

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>
#include <cstdlib>
#include <unistd.h>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <algorithm>
#include <chrono>
#include <dlfcn.h>
#include <regex>

#include "llama.h"
#include <zip.h>

namespace fs = std::filesystem;

struct Config {
    std::string pdf_path;
    std::string model_path   = "Desktop/model.gguf";
    std::string output_docx  = "output_translated.docx";
    std::string temp_dir     = "/tmp/pdf_trans_work";
    int         n_ctx        = 8192;
    int         n_threads    = 28;
    int         n_gpu_layers = 99;
    int         max_tokens   = 2048; // تقليلها قليلاً لمنع الهذيان
    int         dpi          = 200;
    bool        use_ocr      = true;
    std::string ocr_lang     = "eng";
};

// ===== أدوات مساعدة =====

std::string exec_command(const std::string& cmd) {
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) return "";
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe.get()) != nullptr) result += buf;
    return result;
}

std::string clean_model_output(const std::string& text) {
    std::string result = text;

    // ✅ إصلاح المصفوفة: إزالة الرموز الغريبة والمقطوعة
    std::vector<std::string> bad_tokens = {
        "<|im_end|>", "<|im_start|>", "<|im_start|>user", "<|im_start|>assistant",
        "[INST]", "[/INST]", "<<SYS>>", "<</SYS>>",
        "</s>", "<s>", "<pad>", "<unk>", "<end_of_turn>", "[Arabic]"
    };
    for (const auto& tok : bad_tokens) {
        size_t pos;
        while ((pos = result.find(tok)) != std::string::npos)
            result.erase(pos, tok.size());
    }

    size_t start = result.find_first_not_of(" \t\n\r");
    size_t end   = result.find_last_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    result = result.substr(start, end - start + 1);

    result = std::regex_replace(result, std::regex(R"(\n{3,})"), "\n\n");
    return result;
}

std::string clean_extracted_text(const std::string& text) {
    std::string cleaned = text;
    cleaned = std::regex_replace(cleaned, std::regex(R"(https?://\S+)"), "");
    cleaned = std::regex_replace(cleaned, std::regex(R"(\b\d{1,2}/\d{1,2}/\d{2,4}\b)"), "");
    cleaned = std::regex_replace(cleaned, std::regex(R"(\n\s*\d+ of \d+\s*\n)"), "\n");
    cleaned = std::regex_replace(cleaned, std::regex(R"(\n{3,})"), "\n\n");
    while (cleaned.find("  ") != std::string::npos)
        cleaned.replace(cleaned.find("  "), 2, " ");
    size_t s = cleaned.find_first_not_of(" \t\n\r");
    size_t e = cleaned.find_last_not_of(" \t\n\r");
    if (s == std::string::npos) return "";
    return cleaned.substr(s, e - s + 1);
}

// ===== استخراج النص =====

std::string extract_text_from_pdf(const std::string& pdf_path, int page_num) {
    std::string out_file = "/tmp/page_text_" + std::to_string(page_num) + ".txt";
    std::string cmd = "pdftotext -f " + std::to_string(page_num) +
                      " -l " + std::to_string(page_num) +
                      " -layout \"" + pdf_path + "\" \"" + out_file + "\" 2>/dev/null";
    system(cmd.c_str());
    std::ifstream f(out_file);
    if (!f.is_open()) return "";
    std::stringstream ss; ss << f.rdbuf();
    fs::remove(out_file);
    return clean_extracted_text(ss.str());
}

std::string extract_text_with_ocr(const std::string& image_path, const std::string& lang = "eng") {
    std::string out_base = "/tmp/ocr_result_" + std::to_string(getpid());
    std::string cmd = "tesseract \"" + image_path + "\" \"" + out_base + "\" -l " + lang + " 2>/dev/null";
    system(cmd.c_str());
    std::string out_file = out_base + ".txt";
    std::ifstream f(out_file);
    if (!f.is_open()) return "";
    std::stringstream ss; ss << f.rdbuf();
    fs::remove(out_file);
    return clean_extracted_text(ss.str());
}

std::string extract_best_text(const std::string& pdf_path, const std::string& image_path,
                               int page_num, const Config& cfg) {
    std::string pdf_text = extract_text_from_pdf(pdf_path, page_num);
    if (pdf_text.size() > 50) {
        std::cout << "   📄 نص مستخرج من PDF (" << pdf_text.size() << " حرف)\n";
        return pdf_text;
    }

    if (cfg.use_ocr && !image_path.empty() && fs::exists(image_path)) {
        std::cout << "   🔍 النص قصير، جاري تطبيق OCR...\n";
        std::string ocr_text = extract_text_with_ocr(image_path, cfg.ocr_lang);
        if (ocr_text.size() > 20) {
            std::cout << "   ✅ OCR استخرج (" << ocr_text.size() << " حرف)\n";
            return ocr_text;
        }
    }

    if (!pdf_text.empty()) return pdf_text;
    return "";
}

int get_pdf_page_count(const std::string& pdf_path) {
    std::string cmd = "pdfinfo \"" + pdf_path + "\" 2>/dev/null | grep 'Pages:' | awk '{print $2}'";
    std::string result = exec_command(cmd);
    try { return std::stoi(result); } catch (...) { return 0; }
}

std::string pdf_page_to_image(const std::string& pdf_path, int page_num,
                               const std::string& out_dir, int dpi = 200) {
    std::string prefix = out_dir + "/page";
    std::string cmd = "pdftoppm -jpeg -r " + std::to_string(dpi) +
                      " -f " + std::to_string(page_num) +
                      " -l " + std::to_string(page_num) +
                      " \"" + pdf_path + "\" \"" + prefix + "\" 2>/dev/null";
    system(cmd.c_str());

    for (auto& entry : fs::directory_iterator(out_dir)) {
        std::string fname = entry.path().filename().string();
        if (fname.find("page") == 0 && fname.find(".jpg") != std::string::npos) {
            std::string new_name = out_dir + "/pg_" + std::to_string(page_num) + ".jpg";
            if (entry.path().string() != new_name) fs::rename(entry.path(), new_name);
            return new_name;
        }
    }
    return "";
}

// ===== النموذج =====

class LlamaTranslator {
public:
    llama_model*       model = nullptr;
    llama_context*     ctx   = nullptr;
    const llama_vocab* vocab = nullptr;
    int                context_size;
    int                n_batch;

    LlamaTranslator(const Config& cfg) : n_batch(512), context_size(8192) { // ✅ تقليل الـ batch لمنع تكدس CUDA
        std::cout << "⏳ جاري تحميل النموذج: " << cfg.model_path << "\n";

        dlopen("/home/m/llama.cpp/build/src/libllama.so", RTLD_NOW | RTLD_GLOBAL);
        llama_backend_init();

        llama_model_params mparams = llama_model_default_params();
        mparams.n_gpu_layers = cfg.n_gpu_layers;

        model = llama_model_load_from_file(cfg.model_path.c_str(), mparams);
        if (!model) throw std::runtime_error("❌ فشل تحميل النموذج: " + cfg.model_path);

        vocab = llama_model_get_vocab(model);

        llama_context_params cparams = llama_context_default_params();
        context_size = cfg.n_ctx;
        cparams.n_ctx     = context_size;
        cparams.n_threads = cfg.n_threads;
        cparams.n_batch   = n_batch;
        cparams.n_ubatch  = n_batch;

        ctx = llama_init_from_model(model, cparams);
        if (!ctx) throw std::runtime_error("❌ فشل إنشاء السياق");

        std::cout << "✅ تم تحميل النموذج بنجاح\n";
    }

    ~LlamaTranslator() {
        if (ctx)   llama_free(ctx);
        if (model) llama_model_free(model);
        llama_backend_free();
    }

    std::string translate(const std::string& text, int max_tokens = 1024) {
        if (text.empty()) return "";

        std::string full_prompt =
            "<|im_start|>system\n"
            "You are a professional translator. Translate the following text to Arabic. "
            "Output ONLY the Arabic translation. No notes, no explanations.\n"
            "<|im_end|>\n"
            "<|im_start|>user\n"
            + text + "\n"
            "<|im_end|>\n"
            "<|im_start|>assistant\n";

        std::vector<llama_token> tokens(full_prompt.size() + 64);
        int n = llama_tokenize(vocab, full_prompt.c_str(), full_prompt.size(),
                               tokens.data(), tokens.size(), true, true);
        if (n < 0) {
            tokens.resize(-n + 64);
            n = llama_tokenize(vocab, full_prompt.c_str(), full_prompt.size(),
                               tokens.data(), tokens.size(), true, true);
        }
        if (n <= 0) return "";

        // ✅ حماية: إذا الـ prompt أكبر من الـ context، لا تكمل
        if (n > context_size - 256) {
            std::cerr << "   ⚠️ النص أطول من سعة الذاكرة المتاحة، سيتم اقتطاعه بشدة.\n";
            n = context_size - 256;
            tokens.resize(n);
        }

        llama_memory_clear(llama_get_memory(ctx), true);

        // ✅ معالجة على دفعات صغيرة لتجنب تكدس الـ CUDA Graphs
        for (int i = 0; i < n; i += n_batch) {
            int batch_size = std::min(n_batch, n - i);
            llama_batch b = llama_batch_get_one(tokens.data() + i, batch_size);
            if (llama_decode(ctx, b) != 0) return "";
        }

        llama_sampler* sampler = llama_sampler_init_greedy();
        const llama_token eos  = llama_vocab_eos(vocab);

        std::string result;
        size_t max_output_length = text.size() * 3; // ✅ كبح الإخراج: لا يتجاوز 3 أضعاف النص الأصلي
        if (max_output_length < 500) max_output_length = 500; 

        auto start_time = std::chrono::steady_clock::now();
        const int TIMEOUT_SEC = 90; // ✅ مهلة أقصى 90 ثانية للدفعة الواحدة

        for (int i = 0; i < max_tokens; i++) {
            // ✅ فحص المهلة الزمنية لمنع العلق التام
            auto now = std::chrono::steady_clock::now();
            double elapsed_sec = std::chrono::duration<double>(now - start_time).count();
            if (elapsed_sec > TIMEOUT_SEC) {
                std::cerr << "\n   ⏱️ تم تجاوز المهلة الزمنية (" << TIMEOUT_SEC << " ثانية)، تم الإيقاف.\n";
                break;
            }

            // ✅ فحص طول الإخراج
            if (result.size() > max_output_length) {
                std::cerr << "\n   ✂️ الإخراج طويل جداً، تم الإيقاف المبكر.\n";
                break;
            }

            llama_token new_token = llama_sampler_sample(sampler, ctx, -1);
            llama_sampler_accept(sampler, new_token);

            if (new_token == eos) break;

            char buf[256];
            int len = llama_token_to_piece(vocab, new_token, buf, sizeof(buf), 0, true);
            if (len > 0) {
                std::string piece(buf, len);
                if (piece.find("<|im_end|>") != std::string::npos ||
                    piece.find("<|im_start|>") != std::string::npos) break;
                result.append(piece);
            }

            llama_batch next = llama_batch_get_one(&new_token, 1);
            if (llama_decode(ctx, next) != 0) break;
        }

        llama_sampler_free(sampler);
        return clean_model_output(result);
    }

    // ✅ تقسيم أصغر بكثير لضمان الاستقرار
    std::string translate_long(const std::string& text, int max_tokens = 1024) {
        const size_t MAX_CHUNK = 1500; // ✅ تصغير الدفعة إلى 500 حرف فقط
        if (text.size() <= MAX_CHUNK) {
            return translate(text, max_tokens);
        }

        std::cout << "   📑 النص طويل، سيتم تقسيمه إلى دفعات صغيرة...\n";

        std::vector<std::string> chunks;
        std::string current;
        std::stringstream ss(text);
        std::string line;

        while (std::getline(ss, line)) {
            if (current.size() + line.size() + 1 > MAX_CHUNK && !current.empty()) {
                chunks.push_back(current);
                current.clear();
            }
            current += line + "\n";
        }
        if (!current.empty()) chunks.push_back(current);

        std::string full_translation;
        for (size_t i = 0; i < chunks.size(); i++) {
            std::cout << "   🔄 دفعة " << (i+1) << "/" << chunks.size() << "...";
            std::string part = translate(chunks[i], 600); // 600 token لكل 500 حرف هي نسبة آمنة
            std::cout << " تم.\n";
            
            if (!full_translation.empty()) full_translation += "\n";
            full_translation += part;
        }

        return full_translation;
    }
};

// ===== بناء DOCX (بدون تغيير، يعمل بشكل ممتاز) =====

class DocxBuilder {
public:
    struct Page {
        std::string image_path;
        std::string arabic_text;
        int         page_num;
    };
    std::vector<Page> pages;

    void add_page(int num, const std::string& img, const std::string& trans) {
        pages.push_back({img, trans, num});
    }

    static std::string xml_escape(const std::string& s) {
        std::string out;
        out.reserve(s.size() * 1.1);
        for (unsigned char c : s) {
            if      (c == '&')  out += "&amp;";
            else if (c == '<')  out += "&lt;";
            else if (c == '>')  out += "&gt;";
            else if (c == '"')  out += "&quot;";
            else if (c == '\'') out += "&apos;";
            else if (c < 0x09 || (c >= 0x0B && c <= 0x0C) || (c >= 0x0E && c <= 0x1F)) continue;
            else out += (char)c;
        }
        return out;
    }

    bool build(const std::string& output_path) {
        int errorp = 0;
        zip_t* archive = zip_open(output_path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &errorp);
        if (!archive) { std::cerr << "❌ فشل إنشاء ملف DOCX\n"; return false; }

        auto add_str = [&](const char* name, const std::string& content) {
            char* buf = new char[content.size()];
            memcpy(buf, content.c_str(), content.size());
            zip_source_t* s = zip_source_buffer(archive, buf, content.size(), 1);
            zip_file_add(archive, name, s, ZIP_FL_OVERWRITE);
        };

        add_str("[Content_Types].xml",
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
            "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
            "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
            "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
            "<Default Extension=\"jpg\" ContentType=\"image/jpeg\"/>"
            "<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
            "</Types>");

        add_str("_rels/.rels",
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
            "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
            "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/>"
            "</Relationships>");

        std::string word_rels =
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
            "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n";
        for (const auto& p : pages)
            word_rels += "<Relationship Id=\"rId" + std::to_string(p.page_num) +
                "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\""
                " Target=\"media/page_" + std::to_string(p.page_num) + ".jpg\"/>\n";
        word_rels += "</Relationships>";
        add_str("word/_rels/document.xml.rels", word_rels);

        const int PAGE_W  = 23811;
        const int MARGIN  = 720;
        const int GAP     = 200;
        const int COL_W   = (PAGE_W - 2 * MARGIN - GAP) / 2;
        const long long IMG_W_EMU = (long long)COL_W * 635;
        const long long IMG_H_EMU = (long long)(IMG_W_EMU * 1.414);

        std::string doc =
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
            "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\""
            " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\""
            " xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\""
            " xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\""
            " xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\">"
            "<w:body>";

        for (size_t i = 0; i < pages.size(); i++) {
            const auto& p   = pages[i];
            std::string rid = "rId" + std::to_string(p.page_num);

            if (i > 0) doc += "<w:p><w:r><w:br w:type=\"page\"/></w:r></w:p>";

            doc += "<w:p><w:pPr><w:jc w:val=\"center\"/><w:spacing w:after=\"100\"/></w:pPr>"
                   "<w:r><w:rPr><w:b/><w:sz w:val=\"22\"/><w:color w:val=\"444444\"/></w:rPr>"
                   "<w:t>&#x635;&#x641;&#x62D;&#x629; " + std::to_string(p.page_num) + "</w:t></w:r></w:p>";

            doc += "<w:tbl><w:tblPr>"
                   "<w:tblW w:w=\"" + std::to_string(PAGE_W - 2*MARGIN) + "\" w:type=\"dxa\"/>"
                   "<w:tblBorders>"
                   "<w:top w:val=\"single\" w:sz=\"4\" w:color=\"CCCCCC\"/>"
                   "<w:bottom w:val=\"single\" w:sz=\"4\" w:color=\"CCCCCC\"/>"
                   "<w:left w:val=\"single\" w:sz=\"4\" w:color=\"CCCCCC\"/>"
                   "<w:right w:val=\"single\" w:sz=\"4\" w:color=\"CCCCCC\"/>"
                   "<w:insideH w:val=\"single\" w:sz=\"2\" w:color=\"DDDDDD\"/>"
                   "<w:insideV w:val=\"single\" w:sz=\"2\" w:color=\"DDDDDD\"/>"
                   "</w:tblBorders><w:tblLayout w:type=\"fixed\"/></w:tblPr>"
                   "<w:tblGrid>"
                   "<w:gridCol w:w=\"" + std::to_string(COL_W) + "\"/>"
                   "<w:gridCol w:w=\"" + std::to_string(COL_W) + "\"/>"
                   "</w:tblGrid><w:tr>";

            doc += "<w:tc><w:tcPr><w:tcW w:w=\"" + std::to_string(COL_W) + "\" w:type=\"dxa\"/>"
                   "<w:tcMar><w:top w:w=\"80\" w:type=\"dxa\"/><w:bottom w:w=\"80\" w:type=\"dxa\"/>"
                   "<w:left w:w=\"80\" w:type=\"dxa\"/><w:right w:w=\"80\" w:type=\"dxa\"/>"
                   "</w:tcMar></w:tcPr>";

            if (!p.image_path.empty() && fs::exists(p.image_path)) {
                doc += "<w:p><w:pPr><w:jc w:val=\"center\"/></w:pPr><w:r>"
                       "<w:drawing><wp:inline distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\">"
                       "<wp:extent cx=\"" + std::to_string(IMG_W_EMU) + "\" cy=\"" + std::to_string(IMG_H_EMU) + "\"/>"
                       "<wp:docPr id=\"" + std::to_string(p.page_num) + "\" name=\"img" + std::to_string(p.page_num) + "\"/>"
                       "<a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/picture\">"
                       "<pic:pic><pic:nvPicPr>"
                       "<pic:cNvPr id=\"" + std::to_string(p.page_num) + "\" name=\"img" + std::to_string(p.page_num) + "\"/>"
                       "<pic:cNvPicPr/></pic:nvPicPr>"
                       "<pic:blipFill><a:blip r:embed=\"" + rid + "\"/>"
                       "<a:stretch><a:fillRect/></a:stretch></pic:blipFill>"
                       "<pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/>"
                       "<a:ext cx=\"" + std::to_string(IMG_W_EMU) + "\" cy=\"" + std::to_string(IMG_H_EMU) + "\"/>"
                       "</a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>"
                       "</pic:spPr></pic:pic></a:graphicData></a:graphic>"
                       "</wp:inline></w:drawing></w:r></w:p>";
            } else {
                doc += "<w:p><w:r><w:t>(لا توجد صورة)</w:t></w:r></w:p>";
            }
            doc += "</w:tc>";

            doc += "<w:tc><w:tcPr><w:tcW w:w=\"" + std::to_string(COL_W) + "\" w:type=\"dxa\"/>"
                   "<w:shd w:val=\"clear\" w:fill=\"FAFAFA\"/>"
                   "<w:tcMar><w:top w:w=\"100\" w:type=\"dxa\"/><w:bottom w:w=\"100\" w:type=\"dxa\"/>"
                   "<w:left w:w=\"150\" w:type=\"dxa\"/><w:right w:w=\"150\" w:type=\"dxa\"/>"
                   "</w:tcMar></w:tcPr>";

            std::string escaped = xml_escape(p.arabic_text);
            std::stringstream text_ss(escaped);
            std::string line;
            bool first_para = true;

            while (std::getline(text_ss, line)) {
                size_t ls = line.find_first_not_of(" \t");
                if (ls != std::string::npos) line = line.substr(ls);

                doc += "<w:p><w:pPr><w:jc w:val=\"right\"/><w:bidi/>"
                       "<w:spacing w:after=\"60\"" + std::string(first_para ? " w:before=\"0\"" : "") + "/>"
                       "<w:rPr><w:rtl/></w:rPr></w:pPr>";

                if (!line.empty()) {
                    doc += "<w:r><w:rPr><w:rtl/><w:lang w:bidi=\"ar-SA\"/><w:sz w:val=\"22\"/></w:rPr>"
                           "<w:t xml:space=\"preserve\">" + line + "</w:t></w:r>";
                }
                doc += "</w:p>";
                first_para = false;
            }

            if (p.arabic_text.empty()) {
                doc += "<w:p><w:pPr><w:jc w:val=\"center\"/></w:pPr>"
                       "<w:r><w:rPr><w:color w:val=\"999999\"/></w:rPr>"
                       "<w:t>(لا يوجد نص في هذه الصفحة)</w:t></w:r></w:p>";
            }

            doc += "</w:tc></w:tr></w:tbl>";
        }

        doc += "<w:sectPr><w:pgSz w:w=\"23811\" w:h=\"16838\" w:orient=\"landscape\"/>"
               "<w:pgMar w:top=\"720\" w:right=\"720\" w:bottom=\"720\" w:left=\"720\"/>"
               "</w:sectPr></w:body></w:document>";

        add_str("word/document.xml", doc);

        for (const auto& p : pages) {
            if (!p.image_path.empty() && fs::exists(p.image_path)) {
                zip_source_t* img_s = zip_source_file(archive, p.image_path.c_str(), 0, -1);
                if (img_s) {
                    std::string img_name = "word/media/page_" + std::to_string(p.page_num) + ".jpg";
                    zip_file_add(archive, img_name.c_str(), img_s, ZIP_FL_OVERWRITE);
                }
            }
        }

        if (zip_close(archive) != 0) {
            std::cerr << "❌ فشل حفظ ملف DOCX\n";
            return false;
        }
        return true;
    }
};

// ===== البرنامج الرئيسي =====

int main(int argc, char* argv[]) {
    setlocale(LC_ALL, "ar_EG.UTF-8");

    std::cout << "========================================\n"
              << "  مترجم PDF إلى العربية v3 (مستقر)\n"
              << "========================================\n\n";

    Config cfg;

    if (argc < 2) {
        std::cerr << "الاستخدام: " << argv[0] << " <ملف PDF> [نموذج GGUF] [ملف الخرج] [لغة OCR]\n";
        return 1;
    }

    cfg.pdf_path = argv[1];
    if (argc >= 3) cfg.model_path  = argv[2];
    if (argc >= 4) cfg.output_docx = argv[3];
    if (argc >= 5) cfg.ocr_lang    = argv[4];

    if (!fs::exists(cfg.pdf_path))   { std::cerr << "❌ الملف غير موجود: "   << cfg.pdf_path   << "\n"; return 1; }
    if (!fs::exists(cfg.model_path)) { std::cerr << "❌ النموذج غير موجود: " << cfg.model_path << "\n"; return 1; }

    std::string tess_check = exec_command("which tesseract 2>/dev/null");
    if (tess_check.empty()) {
        std::cout << "⚠️  tesseract غير موجود - سيتم استخدام pdftotext فقط\n";
        cfg.use_ocr = false;
    }

    fs::create_directories(cfg.temp_dir);

    int total_pages = get_pdf_page_count(cfg.pdf_path);
    if (total_pages <= 0) {
        std::cerr << "❌ تعذر قراءة PDF أو الملف فارغ\n";
        return 1;
    }
    std::cout << "📄 عدد الصفحات: " << total_pages << "\n\n";

    std::unique_ptr<LlamaTranslator> translator;
    try {
        translator = std::make_unique<LlamaTranslator>(cfg);
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }

    DocxBuilder docx;

    for (int pg = 1; pg <= total_pages; pg++) {
        std::cout << "\n━━━ صفحة " << pg << " / " << total_pages << " ━━━\n";

        std::string img_path = pdf_page_to_image(cfg.pdf_path, pg, cfg.temp_dir, cfg.dpi);
        if (img_path.empty())
            std::cerr << "⚠️  فشل تحويل صفحة " << pg << " إلى صورة\n";

        std::string text = extract_best_text(cfg.pdf_path, img_path, pg, cfg);

        std::string translation;
        if (!text.empty()) {
            auto t0 = std::chrono::steady_clock::now();
            translation = translator->translate_long(text, cfg.max_tokens);
            auto t1 = std::chrono::steady_clock::now();
            double secs = std::chrono::duration<double>(t1 - t0).count();
            std::cout << "   ✅ تمت الترجمة في " << (int)secs << " ثانية (" << translation.size() << " حرف)\n";
            
            std::string preview = translation.substr(0, std::min((size_t)80, translation.size()));
            std::cout << "   [معاينة]: " << preview << (translation.size() > 80 ? "..." : "") << "\n";
        }

        docx.add_page(pg, img_path, translation);
    }

    std::cout << "\n📦 جاري إنشاء ملف DOCX...\n";
    if (docx.build(cfg.output_docx)) {
        std::cout << "\n🎉 اكتملت الترجمة!\n   الملف: " << cfg.output_docx << "\n";
        fs::remove_all(cfg.temp_dir);
    } else {
        return 1;
    }

    return 0;
}
