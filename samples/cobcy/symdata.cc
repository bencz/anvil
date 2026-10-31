// This file is part of cobcy, a COBOL-to-C compiler.
//
// Copyright (C) 1995-2008 by Mike Sharov <msharov@users.sourceforge.net>
// This file is free software, distributed under the MIT License.

#include "symdata.h"
#include "symfile.h"

CobolData::CobolData (void)
: CSize (0)
, DeclLevel (0)
, AssociatedStream (nullptr)
{
}

void CobolData::AssociateWithStream (const CobolFile* NewStream)
{
    AssociatedStream = NewStream;
}

void CobolData::GenRead (ostream& os) const
{
    GenRead (os, AssociatedStream->GetFullCName().c_str());
}

void CobolData::GenWrite (ostream& os) const
{
    GenWrite (os, AssociatedStream->GetFullCName().c_str());
}
