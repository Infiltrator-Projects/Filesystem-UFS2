// SPDX-License-Identifier: GPL-3.0-or-later
// Native PDF compiler for UFS2's original editable page streams.
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
using std::string;
static string bytes(const string &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Unable to read page source: " + path);
    return string(std::istreambuf_iterator<char>(input), {});
}
static void replace_version(string &source, const string &version) {
    static const string token = "@VERSION@";
    size_t index = 0;
    while ((index = source.find(token, index)) != string::npos) {
        source.replace(index, token.size(), version);
        index += version.size();
    }
}
int main(int argc, char **argv) {
    if (argc != 4) {
        std::cerr << "usage: manual PAGE_SOURCE PDF_OUTPUT VERSION\n";
        return 2;
    }
    try {
        string source = bytes(argv[1]);
        string upper = source;
        std::transform(upper.begin(), upper.end(), upper.begin(),
            [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (source.find("https://") == string::npos ||
            source.find("https://", source.find("https://") + 8) == string::npos ||
            upper.find("HISTORICAL") == string::npos)
            throw std::runtime_error("Missing original history and primary sources");
        replace_version(source, argv[3]);
        const string separator = "\n%%Page\n";
        std::vector<string> pages;
        size_t beginning = 0;
        for (;;) {
            size_t next = source.find(separator, beginning);
            pages.push_back(source.substr(beginning,
                next == string::npos ? string::npos : next - beginning));
            if (next == string::npos) break;
            beginning = next + separator.size();
        }
        if (pages.size() < 8) throw std::runtime_error("UFS2 manual needs at least eight pages");
        std::vector<string> objects = {
            "<< /Type /Catalog /Pages 2 0 R >>", "",
            "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
            "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold >>",
            "<< /Type /Font /Subtype /Type1 /BaseFont /Courier >>"
        };
        string children;
        for (auto &page : pages) {
            const size_t index = objects.size() + 1;
            children += std::to_string(index) + " 0 R ";
            objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] "
                "/Resources << /Font << /F1 3 0 R /F2 4 0 R /F3 5 0 R >> >> "
                "/Contents " + std::to_string(index + 1) + " 0 R >>");
            while (!page.empty() && page.back() == '\n') page.pop_back();
            page += '\n';
            objects.push_back("<< /Length " + std::to_string(page.size()) +
                " >>\nstream\n" + page + "endstream");
        }
        objects[1] = "<< /Type /Pages /Kids [" + children +
            "] /Count " + std::to_string(pages.size()) + " >>";
        std::ofstream out(argv[2], std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("Cannot open PDF destination");
        out << "%PDF-1.4\n%\342\343\317\323\n";
        std::vector<std::streamoff> offsets;
        for (size_t i = 0; i < objects.size(); ++i) {
            offsets.push_back(static_cast<std::streamoff>(out.tellp()));
            out << i + 1 << " 0 obj\n" << objects[i] << "\nendobj\n";
        }
        const auto xref = static_cast<std::streamoff>(out.tellp());
        out << "xref\n0 " << objects.size() + 1
            << "\n0000000000 65535 f \n";
        for (const auto offset : offsets)
            out << std::setw(10) << std::setfill('0') << offset << " 00000 n \n";
        out << "trailer\n<< /Size " << objects.size() + 1
            << " /Root 1 0 R >>\nstartxref\n" << xref << "\n%%EOF\n";
        if (!out) throw std::runtime_error("Writing PDF failed");
    } catch (const std::exception &e) {
        std::cerr << "UFS2 PDF generation failed: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
