#pragma once
#include <fstream>
#include <vector>

namespace reach {
// Bounded per-stream buffer. Keep routine filesystem writes outside the 60 s
// measurement window; explicit close/flush at completion still reports IO errors.
// A forced process termination can lose the last buffer: never certify that run.
class BufferedLog : public std::ofstream {
public:
    BufferedLog():buffer_(4*1024*1024) { rdbuf()->pubsetbuf(buffer_.data(),static_cast<std::streamsize>(buffer_.size())); }
    ~BufferedLog() { try { if(is_open())close(); } catch(...) {} }
private:
    std::vector<char> buffer_;
};
}
