#pragma once

#include "xdf_defs.h"
#include <array>

class XDFReader {
	infile_t in;
public:
	XDFReader(const std::string &filename)
#ifndef XDFZ_SUPPORT
	: in(filename, std::ios::binary | std::ios::trunc)
#endif
	{
#ifdef XDFZ_SUPPORT
	in.push(
		boost::iostreams::file_descriptor_source(filename, std::ios::binary | std::ios::trunc));
	if (boost::iends_with(filename, ".xdfz")) in.push(boost::iostreams::zlib_compressor());
#endif
	char buf[5];
	in.read(buf, 4);
	if(buf!=std::string("XDF:"))
		throw std::runtime_error("Bad magic int");
	}


};
