// Copyright (c) 2014, Facebook, Inc.  All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.

package org.rocksdb;

/**
 * Base class for slices which will receive
 * byte[] based access to the underlying data.
 *
 * byte[] backed slices typically perform better with
 * small keys and values. When using larger keys and
 * values consider using @see org.rocksdb.DirectSlice
 */
public class Slice extends AbstractSlice<byte[]> {
  /**
   * Called from JNI to construct a new Java Slice
   * without an underlying C++ object set
   * at creation time.
   *
   * Note: You should be aware that
   * {@see org.rocksdb.RocksObject#disOwnNativeHandle()} is intentionally
   * called from the default Slice constructor, and that it is marked as
   * private. This is so that developers cannot construct their own default
   * Slice objects (at present). As developers cannot construct their own
   * Slice objects through this, they are not creating underlying C++ Slice
   * objects, and so there is nothing to free (dispose) from Java.
   */
  private Slice() {
    super();
    disOwnNativeHandle();
  }

  /**
   * Constructs a slice
   * where the data is taken from
   * a String.
   */
  public Slice(final String str) {
    super();
    createNewSliceFromString(str);
  }

  /**
   * Constructs a slice
   * where the data is a copy of
   * the byte array from a specific offset.
   */
  public Slice(final byte[] data, final int offset) {
    super();
    createNewSlice0(data, offset);
  }

  /**
   * Constructs a slice
   * where the data is a copy of
   * the byte array.
   */
  public Slice(final byte[] data) {
    super();
    createNewSlice1(data);
  }

  /**
   * Deletes underlying C++ slice pointer
   * and any buffered data.
   *
   * <p/>
   * Note that this function should be called only after all
   * RocksDB instances referencing the slice are closed.
   * Otherwise an undefined behavior will occur.
   */
  @Override
  protected void disposeInternal() {
    super.disposeInternal();
    disposeInternalBuf(nativeHandle_);
  }

  @Override protected final native byte[] data0(long handle);
  private native void createNewSlice0(byte[] data, int length);
  private native void createNewSlice1(byte[] data);
  private native void disposeInternalBuf(long handle);
}
