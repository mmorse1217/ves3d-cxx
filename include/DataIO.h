/**
 * @file   DataIO.h
 * @author Rahimian, Abtin <arahimian@acm.org>
 * @date   Thu Feb  4 13:11:15 2010
 * 
 * @brief  The data I/O class header.
 */

#ifndef _DATAIO_H_
#define _DATAIO_H_

#include <stdio.h>
#include <stdlib.h>
#include <iostream>
#include <fstream>
#include <string>
#include <cassert>
#include "Logger.h"
#include "enums.h"
#include <memory>
/**
 * A simple data I/O class tailored for this project. It reads data
 * from and writes data to file. 
 */
using namespace std;

class DataIO
{
  public:
    explicit DataIO(string file_name = "", size_t buffer_size = 0, 
        int resize_factor = 2);    
    ~DataIO();
    
    template<typename Container>
    bool ReadData(const string &file_name, Container &data, 
        int offset = 0 , int length = -1) const;
    
    template<typename Container>
    bool WriteData(const string &file_name, const Container &data, 
        ios_base::openmode mode = ios::out) const;
    
    template<typename Container>
    bool Append(const Container &data) const; 

    bool ResizeOutBuffer(size_t buffer_size) const;
    bool FlushBuffer() const;
    
    template<typename T>
    bool FlushBuffer() const;

  private:
    // Basic type IO
    template<typename T>
    bool ReadData(const string &file_name, size_t size, T* data) const;
    bool ReadData(const string &file_name, size_t size, char* data) const;

    
    template<typename T>
    bool WriteData(const string &file_name, size_t size, const T* data, 
        ios_base::openmode mode = ios::out) const;
    
    bool WriteData(const string &file_name, size_t size, const char* data, 
        ios_base::openmode mode) const;

    string out_file_;
    mutable size_t out_size_;
    mutable size_t out_used_;
    mutable char* out_buffer_; 
    int resize_factor_;
};

#include "DataIO_templates.cc"

#endif

    

