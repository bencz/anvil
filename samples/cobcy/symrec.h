// This file is part of cobcy, a COBOL-to-C compiler.
//
// Copyright (C) 1995-2008 by Mike Sharov <msharov@users.sourceforge.net>
// This file is free software, distributed under the MIT License.

#pragma once
#include "symdata.h"
#include "symfile.h"

/// Defines a cobcy record symbol
class CobolRecord : public CobolData {
public:
				CobolRecord (void);
    void			AddChild (const CobolData* NewChild);
    virtual CobolSymbolType	Kind (void) const override { return CS_Record; }
    void			GenDeclareBegin (ostream& os) const;
    void			GenDeclareEnd (ostream& os) const;
    virtual void		GenRead (ostream& os, const char* stream) const override;
    virtual void		GenWrite (ostream& os, const char* stream) const override;
    virtual void		GenSignature (ostream& os) const override;
private:
    vector<const CobolData*>	_fields;
};
