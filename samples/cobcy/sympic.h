// This file is part of cobcy, a COBOL-to-C compiler.
//
// Copyright (C) 1995-2008 by Mike Sharov <msharov@users.sourceforge.net>
// This file is free software, distributed under the MIT License.

#pragma once
#include "symbase.h"

/// Defines a picture data type. Here are all the data-type dependent functions.
class PictureType : public CobolSymbol {
public:
			PictureType (void);
    uint32_t		Set (const char* NewPicture);
    void		GenTypePrefix (ostream& os) const;
    void		GenTypeSuffix (ostream& os) const;
    void		GenReadFunction (ostream& os) const;
    void		GenWriteFunction (ostream& os) const;
    void		GenSignature (ostream& os) const;
    bool		GenCastFrom (ostream& os, const PictureType& pic) const;
    bool		GenCastTo (ostream& os, const PictureType& pic) const;
    virtual void	text_write (ostream& os) const override;
    virtual CobolSymbolType	Kind (void) const override { return CS_Picture; }
    inline bool		IsNumeric (void) const	{ return _kind == Integer || _kind == Float; }
    inline uint32_t	GetSize (void) const	{ return Size; }
protected:
    static void		Expand (const char* pic, string& expanded);
protected:
    enum EKind { 
	Undefined,
	Integer,
	Float,
	String
    };
    enum ESignPos {
	NoSign = false,
	TrailingSign = true,
	LeadingSign
    };
protected:
    uint32_t		Size;
    uint32_t		CSize;
    EKind		_kind;
    ESignPos		Sign;
    bool		SignSeparate;
    uint32_t		nDigitsBDP;	// Before Decimal Point
    uint32_t		nDigitsADP;	// After Decimal Point
    uint32_t		nFillerZeroes;	// Zeroes after DP
    string		_text;
};
