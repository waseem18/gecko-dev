//
// Copyright (c) 2013-2014 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

// BufferStorage9.h Defines the BufferStorage9 class.

#ifndef LIBGLESV2_RENDERER_BUFFERSTORAGE9_H_
#define LIBGLESV2_RENDERER_BUFFERSTORAGE9_H_

#include "libGLESv2/renderer/BufferStorage.h"

namespace rx
{

class BufferStorage9 : public BufferStorage
{
  public:
    BufferStorage9();
    virtual ~BufferStorage9();

    static BufferStorage9 *makeBufferStorage9(BufferStorage *bufferStorage);

    virtual void *getData();
    virtual void setData(const void* data, size_t size, size_t offset);
    virtual void copyData(BufferStorage* sourceStorage, size_t size, size_t sourceOffset, size_t destOffset);
    virtual void clear();
    virtual void markTransformFeedbackUsage();
    virtual size_t getSize() const;
    virtual bool supportsDirectBinding() const;

    virtual bool isMapped() const;
    virtual void *map(GLbitfield access);
    virtual void unmap();

  private:
    DISALLOW_COPY_AND_ASSIGN(BufferStorage9);

    std::vector<char> mMemory;
    size_t mSize;
};

}

#endif // LIBGLESV2_RENDERER_BUFFERSTORAGE9_H_
