#pragma once
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <ostream>
#include <streambuf>
#include <vector>
namespace reach {
// A hard-bounded journal. No disk writes during insertion, including when the
// usual filebuf would flush. Only the owner writes to a given stream.
class DeferredBuffer:public std::streambuf {
    static constexpr size_t Chunk=65536;
    std::vector<std::unique_ptr<char[]>> blocks_;size_t limit_,allocated_=0;
protected:
    int_type overflow(int_type c)override{
        if(traits_type::eq_int_type(c,traits_type::eof()))return traits_type::not_eof(c);
        if(allocated_>=limit_)return traits_type::eof();
        const auto n=(std::min)(Chunk,limit_-allocated_);auto block=std::make_unique<char[]>(n);auto* p=block.get();blocks_.push_back(std::move(block));allocated_+=n;setp(p,p+n);
        *pptr()=traits_type::to_char_type(c);pbump(1);return c;
    }
public:
    explicit DeferredBuffer(size_t limit):limit_(limit){}
    void Write(std::ostream& out){
        for(size_t i=0;i<blocks_.size();++i){const auto n=i+1==blocks_.size()?size_t(pptr()-pbase()):Chunk;out.write(blocks_[i].get(),std::streamsize(n));}
    }
};
class DeferredLog:public std::ostream {
    DeferredBuffer buffer_;std::ofstream file_;
public:
    explicit DeferredLog(size_t limit=128ull*1024*1024):std::ostream(nullptr),buffer_(limit){rdbuf(&buffer_);}
    void open(const std::filesystem::path& path,std::ios_base::openmode mode=std::ios::out){file_.exceptions(std::ios::badbit|std::ios::failbit);file_.open(path,mode|std::ios::binary);}
    bool is_open()const{return file_.is_open();}
    void close(){if(!file_.is_open())return;buffer_.Write(file_);file_.close();}
    ~DeferredLog(){try{close();}catch(...){}}
};
}
