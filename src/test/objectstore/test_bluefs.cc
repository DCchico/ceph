// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <stdio.h>
#include <string.h>
#include <iostream>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <random>
#include <thread>
#include "global/global_init.h"
#include "common/ceph_argparse.h"
#include "include/stringify.h"
#include "include/scope_guard.h"
#include "common/errno.h"
#include <gtest/gtest.h>

#include "os/bluestore/BlueFS.h"

string get_temp_bdev(uint64_t size)
{
  static int n = 0;
  string fn = "ceph_test_bluefs.tmp.block." + stringify(getpid())
    + "." + stringify(++n);
  int fd = ::open(fn.c_str(), O_CREAT|O_RDWR|O_TRUNC, 0644);
  ceph_assert(fd >= 0);
  int r = ::ftruncate(fd, size);
  ceph_assert(r >= 0);
  ::close(fd);
  return fn;
}

std::unique_ptr<char[]> gen_buffer(uint64_t size)
{
    std::unique_ptr<char[]> buffer = std::make_unique<char[]>(size);
    std::independent_bits_engine<std::default_random_engine, CHAR_BIT, unsigned char> e;
    std::generate(buffer.get(), buffer.get()+size, std::ref(e));
    return buffer;
}


void rm_temp_bdev(string f)
{
  ::unlink(f.c_str());
}

TEST(BlueFS, mkfs) {
  uint64_t size = 1048576 * 128;
  string fn = get_temp_bdev(size);
  uuid_d fsid;
  BlueFS fs(g_ceph_context);
  ASSERT_EQ(0, fs.add_block_device(BlueFS::BDEV_DB, fn, false));
  fs.add_block_extent(BlueFS::BDEV_DB, 1048576, size - 1048576);
  ASSERT_EQ(0, fs.mkfs(fsid));
  rm_temp_bdev(fn);
}

TEST(BlueFS, mkfs_mount) {
  uint64_t size = 1048576 * 128;
  string fn = get_temp_bdev(size);
  BlueFS fs(g_ceph_context);
  ASSERT_EQ(0, fs.add_block_device(BlueFS::BDEV_DB, fn, false));
  fs.add_block_extent(BlueFS::BDEV_DB, 1048576, size - 1048576);
  uuid_d fsid;
  ASSERT_EQ(0, fs.mkfs(fsid));
  ASSERT_EQ(0, fs.mount());
  ASSERT_EQ(fs.get_total(BlueFS::BDEV_DB), size - 1048576);
  ASSERT_LT(fs.get_free(BlueFS::BDEV_DB), size - 1048576);
  fs.umount();
  rm_temp_bdev(fn);
}

TEST(BlueFS, write_read) {
  uint64_t size = 1048576 * 128;
  string fn = get_temp_bdev(size);
  BlueFS fs(g_ceph_context);
  ASSERT_EQ(0, fs.add_block_device(BlueFS::BDEV_DB, fn, false));
  fs.add_block_extent(BlueFS::BDEV_DB, 1048576, size - 1048576);
  uuid_d fsid;
  ASSERT_EQ(0, fs.mkfs(fsid));
  ASSERT_EQ(0, fs.mount());
  {
    BlueFS::FileWriter *h;
    ASSERT_EQ(0, fs.mkdir("dir"));
    ASSERT_EQ(0, fs.open_for_write("dir", "file", &h, false));
    h->append("foo", 3);
    h->append("bar", 3);
    h->append("baz", 3);
    fs.fsync(h);
    fs.close_writer(h);
  }
  {
    BlueFS::FileReader *h;
    ASSERT_EQ(0, fs.open_for_read("dir", "file", &h));
    bufferlist bl;
    BlueFS::FileReaderBuffer buf(4096);
    ASSERT_EQ(9, fs.read(h, &buf, 0, 1024, &bl, NULL));
    ASSERT_EQ(0, strncmp("foobarbaz", bl.c_str(), 9));
    delete h;
  }
  fs.umount();
  rm_temp_bdev(fn);
}

TEST(BlueFS, copy_WR) {
  uint64_t size = 1048576 * 128;
  string fn = get_temp_bdev(size);
  g_ceph_context->_conf->bluefs_alloc_size = 4096;
  BlueFS fs(g_ceph_context);
  ASSERT_EQ(0, fs.add_block_device(BlueFS::BDEV_DB, fn, false));
  fs.add_block_extent(BlueFS::BDEV_DB, 1048576, size - 1048576);
  uuid_d fsid;
  ASSERT_EQ(0, fs.mkfs(fsid));
  ASSERT_EQ(0, fs.mount());
  std::vector<std::vector<uint64_t>> copy;
  std::vector<BlueFS::FileRef> files;
  char data[8192];
  for (unsigned i = 0; i < sizeof(data); ++i)
    data[i] = i;
{
  BlueFS::FileWriter* h;
  ASSERT_EQ(0, fs.mkdir("dir"));
  ASSERT_EQ(0, fs.open_for_write("dir", "file", &h, false));
  h->append(data, sizeof(data));
  fs.fsync(h);
  files.push_back(h->file);
  fs.close_writer(h);
  cerr << "First Write finished" << std::endl;
}
{
  copy.push_back({4096, 4096, 4096});
  BlueFS::FileWriter* h;
  ASSERT_EQ(0, fs.open_for_write("dir", "file_cp", &h, false));
  h->append(data, sizeof(data));
  fs.fsync(h, copy, files);
  fs.close_writer(h);
  cerr << "Copy Write finished" << std::endl;
}
{
	BlueFS::FileReader* h;
	ASSERT_EQ(0, fs.open_for_read("dir", "file_cp", &h));
	bufferlist bl;
	BlueFS::FileReaderBuffer buf(81920);
        fs.read(h, &buf, 0, 8192, &bl, NULL);
	int r = memcmp(data, bl.c_str(), sizeof(data));
	if (r)
	  cerr << "read got mismatch, r = " << r << std::endl;
	ASSERT_EQ(0, r);
	delete h;
}
fs.umount();
rm_temp_bdev(fn);
}
/*
TEST(BlueFS, small_appends) {
  uint64_t size = 1048576 * 128;
  string fn = get_temp_bdev(size);
  BlueFS fs(g_ceph_context);
  ASSERT_EQ(0, fs.add_block_device(BlueFS::BDEV_DB, fn, false));
  fs.add_block_extent(BlueFS::BDEV_DB, 1048576, size - 1048576);
  uuid_d fsid;
  ASSERT_EQ(0, fs.mkfs(fsid));
  ASSERT_EQ(0, fs.mount());
  {
    BlueFS::FileWriter *h;
    ASSERT_EQ(0, fs.mkdir("dir"));
    ASSERT_EQ(0, fs.open_for_write("dir", "file", &h, false));
    for (unsigned i = 0; i < 10000; ++i) {
      h->append("abcdeabcdeabcdeabcdeabcdeabc", 23);
    }
    fs.fsync(h);
    fs.close_writer(h);
  }
  {
    BlueFS::FileWriter *h;
	ASSERT_EQ(0, fs.open_for_write("dir", "file_sync", &h, false));
    for (unsigned i = 0; i < 1000; ++i) {
      h->append("abcdeabcdeabcdeabcdeabcdeabc", 23);
      ASSERT_EQ(0, fs.fsync(h));
    }
    fs.close_writer(h);
  }
  fs.umount();
  rm_temp_bdev(fn);
}
*/ 
TEST(BlueFS, very_large_write) {
  // we'll write a ~3G file, so allocate more than that for the whole fs
  uint64_t size = 1048576 * 1024 * 8ull;
  string fn = get_temp_bdev(size);
  cerr << "l0_granularity = " <<  g_ceph_context->_conf->bluefs_alloc_size << std::endl;
  g_ceph_context->_conf->bluefs_alloc_size = 4096;
  BlueFS fs(g_ceph_context);

  bool old = g_ceph_context->_conf.get_val<bool>("bluefs_buffered_io");
  g_ceph_context->_conf.set_val("bluefs_buffered_io", "false");

  ASSERT_EQ(0, fs.add_block_device(BlueFS::BDEV_DB, fn, false));
  fs.add_block_extent(BlueFS::BDEV_DB, 1048576, size - 1048576);
  uuid_d fsid;
  ASSERT_EQ(0, fs.mkfs(fsid));
  ASSERT_EQ(0, fs.mount());
  char buf[1048576]; // this is biggish, but intentionally not evenly aligned
  for (unsigned i = 0; i < sizeof(buf); ++i) {
    buf[i] = i;
  }
  {
    BlueFS::FileWriter *h;
    ASSERT_EQ(0, fs.mkdir("dir"));
    ASSERT_EQ(0, fs.open_for_write("dir", "bigfile", &h, false));
    for (unsigned i = 0; i < 3*1048576ull / sizeof(buf); ++i) {
      h->append(buf, sizeof(buf));
    }
    fs.fsync(h);
    fs.close_writer(h);
  }
  {
    BlueFS::FileWriter *h;
    ASSERT_EQ(0, fs.open_for_write("dir", "bigfile", &h, true));
    for (unsigned i = 0; i < 3*1048576ull / sizeof(buf); ++i) {
      h->append(buf, sizeof(buf));
    }
    fs.fsync(h);
    fs.close_writer(h);
  }
  {
    BlueFS::FileReader *h;
    ASSERT_EQ(0, fs.open_for_read("dir", "bigfile", &h));
    bufferlist bl;
    BlueFS::FileReaderBuffer readbuf(10485760);
    for (unsigned i = 0; i < 6*1048576ull / 4096; ++i) {
      bl.clear();
      fs.read(h, &readbuf, i * 4096, 4096, &bl, NULL);
      int r = memcmp(buf, bl.c_str(), 4096);
      if (r) {
	cerr << "read got mismatch at offset " << i*4096 << " r " << r
	     << std::endl;
      }
      ASSERT_EQ(0, r);
    }
    delete h;
  }
  fs.umount();

  g_ceph_context->_conf.set_val("bluefs_buffered_io", stringify((int)old));

  rm_temp_bdev(fn);
}

TEST(BlueFS, very_large_copy) {
  // we'll write a ~3G file, so allocate more than that for the whole fs
  uint64_t size = 1048576 * 1024 * 8ull;
  string fn = get_temp_bdev(size);
  BlueFS fs(g_ceph_context);

  bool old = g_ceph_context->_conf.get_val<bool>("bluefs_buffered_io");
  g_ceph_context->_conf.set_val("bluefs_buffered_io", "false");

  ASSERT_EQ(0, fs.add_block_device(BlueFS::BDEV_DB, fn, false));
  fs.add_block_extent(BlueFS::BDEV_DB, 1048576, size - 1048576);
  uuid_d fsid;
  ASSERT_EQ(0, fs.mkfs(fsid));
  ASSERT_EQ(0, fs.mount());
  char buf[1048576]; // this is biggish, but intentionally not evenly aligned
  for (unsigned i = 0; i < sizeof(buf); ++i) {
	buf[i] = i;
  }
  std::vector<std::vector<uint64_t>> copy;
  std::vector<BlueFS::FileRef> files;
{
  BlueFS::FileWriter* h;
  ASSERT_EQ(0, fs.mkdir("dir"));
  ASSERT_EQ(0, fs.open_for_write("dir", "bigfile", &h, false));
  for (unsigned i = 0; i < 1024 * 1048576ull / sizeof(buf); ++i) {
	h->append(buf, sizeof(buf));
  }
  fs.fsync(h);
  //files.push_back(h->file);
  files.push_back(h->file);
  fs.close_writer(h);
  cerr << "First Write finished" << std::endl;
}
{
  //copy.push_back({ 0, 8192, 0 });
  copy.push_back({4096, 4096, 4096});
  BlueFS::FileWriter* h;
  ASSERT_EQ(0, fs.open_for_write("dir", "bigfile_cp", &h, false));
  for (unsigned i = 0; i < 1024 * 1048576ull / sizeof(buf); ++i) {
	h->append(buf, sizeof(buf));
  }
  fs.fsync(h, copy, files);
  fs.close_writer(h);
  cerr << "Second copy write finished!" << std::endl;
}
{
  BlueFS::FileReader* h;
  ASSERT_EQ(0, fs.open_for_read("dir", "bigfile_cp", &h));
  bufferlist bl;
  BlueFS::FileReaderBuffer readbuf(10485760);
  for (unsigned i = 0; i < 1024 * 1048576ull / 4096; ++i) {
	bl.clear();
	fs.read(h, &readbuf, i * 4096, 4096, &bl, NULL);
	int r = memcmp(buf, bl.c_str(), 4096);
	if (r) {
	  cerr << "read got mismatch at offset " << i * 4096 << " r =  " << r
			<< std::endl;
	}
  ASSERT_EQ(0, r);
  }
  delete h;
}
  fs.umount();

  g_ceph_context->_conf.set_val("bluefs_buffered_io", stringify((int)old));

  rm_temp_bdev(fn);
}

/*
#define ALLOC_SIZE 4096

void write_data(BlueFS &fs, uint64_t rationed_bytes)
{
    int j=0, r=0;
    uint64_t written_bytes = 0;
    rationed_bytes -= ALLOC_SIZE;
    stringstream ss;
    string dir = "dir.";
    ss << std::this_thread::get_id();
    dir.append(ss.str());
    dir.append(".");
    dir.append(to_string(j));
    ASSERT_EQ(0, fs.mkdir(dir));
    while (1) {
      string file = "file.";
      file.append(to_string(j));
      BlueFS::FileWriter *h;
      ASSERT_EQ(0, fs.open_for_write(dir, file, &h, false));
      ASSERT_NE(nullptr, h);
      auto sg = make_scope_guard([&fs, h] { fs.close_writer(h); });
      bufferlist bl;
      std::unique_ptr<char[]> buf = gen_buffer(ALLOC_SIZE);
      bufferptr bp = buffer::claim_char(ALLOC_SIZE, buf.get());
      bl.push_back(bp);
      h->append(bl.c_str(), bl.length());
      r = fs.fsync(h);
      if (r < 0) {
         break;
      }
      written_bytes += g_conf()->bluefs_alloc_size;
      j++;
      if ((rationed_bytes - written_bytes) <= g_conf()->bluefs_alloc_size) {
        break;
      }
    }
}

void create_single_file(BlueFS &fs)
{
    BlueFS::FileWriter *h;
    stringstream ss;
    string dir = "dir.test";
    ASSERT_EQ(0, fs.mkdir(dir));
    string file = "testfile";
    ASSERT_EQ(0, fs.open_for_write(dir, file, &h, false));
    bufferlist bl;
    std::unique_ptr<char[]> buf = gen_buffer(ALLOC_SIZE);
    bufferptr bp = buffer::claim_char(ALLOC_SIZE, buf.get());
    bl.push_back(bp);
    h->append(bl.c_str(), bl.length());
    fs.fsync(h);
    fs.close_writer(h);
}

void write_single_file(BlueFS &fs, uint64_t rationed_bytes)
{
    stringstream ss;
    const string dir = "dir.test";
    const string file = "testfile";
    uint64_t written_bytes = 0;
    rationed_bytes -= ALLOC_SIZE;
    while (1) {
      BlueFS::FileWriter *h;
      ASSERT_EQ(0, fs.open_for_write(dir, file, &h, false));
      ASSERT_NE(nullptr, h);
      auto sg = make_scope_guard([&fs, h] { fs.close_writer(h); });
      bufferlist bl;
      std::unique_ptr<char[]> buf = gen_buffer(ALLOC_SIZE);
      bufferptr bp = buffer::claim_char(ALLOC_SIZE, buf.get());
      bl.push_back(bp);
      h->append(bl.c_str(), bl.length());
      int r = fs.fsync(h);
      if (r < 0) {
         break;
      }
      written_bytes += g_conf()->bluefs_alloc_size;
      if ((rationed_bytes - written_bytes) <= g_conf()->bluefs_alloc_size) {
        break;
      }
    }
}

bool writes_done = false;

void sync_fs(BlueFS &fs)
{
    while (1) {
      if (writes_done == true)
        break;
      fs.sync_metadata();
      sleep(1);
    }
}


void do_join(std::thread& t)
{
    t.join();
}

void join_all(std::vector<std::thread>& v)
{
    std::for_each(v.begin(),v.end(),do_join);
}

#define NUM_WRITERS 3
#define NUM_SYNC_THREADS 1

#define NUM_SINGLE_FILE_WRITERS 1
#define NUM_MULTIPLE_FILE_WRITERS 2

TEST(BlueFS, test_flush_1) {
  uint64_t size = 1048576 * 128;
  string fn = get_temp_bdev(size);
  g_ceph_context->_conf.set_val(
    "bluefs_alloc_size",
    "65536");
  g_ceph_context->_conf.apply_changes(nullptr);

  BlueFS fs(g_ceph_context);
  ASSERT_EQ(0, fs.add_block_device(BlueFS::BDEV_DB, fn, false));
  fs.add_block_extent(BlueFS::BDEV_DB, 1048576, size - 1048576);
  uuid_d fsid;
  ASSERT_EQ(0, fs.mkfs(fsid));
  ASSERT_EQ(0, fs.mount());
  {
    std::vector<std::thread> write_thread_multiple;
    uint64_t effective_size = size - (32 * 1048576); // leaving the last 32 MB for log compaction
    uint64_t per_thread_bytes = (effective_size/(NUM_MULTIPLE_FILE_WRITERS + NUM_SINGLE_FILE_WRITERS));
    for (int i=0; i<NUM_MULTIPLE_FILE_WRITERS ; i++) {
      write_thread_multiple.push_back(std::thread(write_data, std::ref(fs), per_thread_bytes));
    }

    create_single_file(fs);
    std::vector<std::thread> write_thread_single;
    for (int i=0; i<NUM_SINGLE_FILE_WRITERS; i++) {
      write_thread_single.push_back(std::thread(write_single_file, std::ref(fs), per_thread_bytes));
    }

    join_all(write_thread_single);
    join_all(write_thread_multiple);
  }
  fs.umount();
  rm_temp_bdev(fn);
}

TEST(BlueFS, test_flush_2) {
  uint64_t size = 1048576 * 256;
  string fn = get_temp_bdev(size);
  g_ceph_context->_conf.set_val(
    "bluefs_alloc_size",
    "65536");
  g_ceph_context->_conf.apply_changes(nullptr);

  BlueFS fs(g_ceph_context);
  ASSERT_EQ(0, fs.add_block_device(BlueFS::BDEV_DB, fn, false));
  fs.add_block_extent(BlueFS::BDEV_DB, 1048576, size - 1048576);
  uuid_d fsid;
  ASSERT_EQ(0, fs.mkfs(fsid));
  ASSERT_EQ(0, fs.mount());
  {
    uint64_t effective_size = size - (128 * 1048576); // leaving the last 32 MB for log compaction
    uint64_t per_thread_bytes = (effective_size/(NUM_WRITERS));
    std::vector<std::thread> write_thread_multiple;
    for (int i=0; i<NUM_WRITERS; i++) {
      write_thread_multiple.push_back(std::thread(write_data, std::ref(fs), per_thread_bytes));
    }

    join_all(write_thread_multiple);
  }
  fs.umount();
  rm_temp_bdev(fn);
}

TEST(BlueFS, test_flush_3) {
  uint64_t size = 1048576 * 256;
  string fn = get_temp_bdev(size);
  g_ceph_context->_conf.set_val(
    "bluefs_alloc_size",
    "65536");
  g_ceph_context->_conf.apply_changes(nullptr);

  BlueFS fs(g_ceph_context);
  ASSERT_EQ(0, fs.add_block_device(BlueFS::BDEV_DB, fn, false));
  fs.add_block_extent(BlueFS::BDEV_DB, 1048576, size - 1048576);
  uuid_d fsid;
  ASSERT_EQ(0, fs.mkfs(fsid));
  ASSERT_EQ(0, fs.mount());
  {
    std::vector<std::thread> write_threads;
    uint64_t effective_size = size - (64 * 1048576); // leaving the last 11 MB for log compaction
    uint64_t per_thread_bytes = (effective_size/(NUM_WRITERS));
    for (int i=0; i<NUM_WRITERS; i++) {
      write_threads.push_back(std::thread(write_data, std::ref(fs), per_thread_bytes));
    }

    std::vector<std::thread> sync_threads;
    for (int i=0; i<NUM_SYNC_THREADS; i++) {
      sync_threads.push_back(std::thread(sync_fs, std::ref(fs)));
    }

    join_all(write_threads);
    writes_done = true;
    join_all(sync_threads);
  }
  fs.umount();
  rm_temp_bdev(fn);
}

TEST(BlueFS, test_simple_compaction_sync) {
  g_ceph_context->_conf.set_val(
    "bluefs_compact_log_sync",
    "true");
  uint64_t size = 1048576 * 128;
  string fn = get_temp_bdev(size);

  BlueFS fs(g_ceph_context);
  ASSERT_EQ(0, fs.add_block_device(BlueFS::BDEV_DB, fn, false));
  fs.add_block_extent(BlueFS::BDEV_DB, 1048576, size - 1048576);
  uuid_d fsid;
  ASSERT_EQ(0, fs.mkfs(fsid));
  ASSERT_EQ(0, fs.mount());
  {
    for (int i=0; i<10; i++) {
       string dir = "dir.";
       dir.append(to_string(i));
       ASSERT_EQ(0, fs.mkdir(dir));
       for (int j=0; j<10; j++) {
          string file = "file.";
	  file.append(to_string(j));
          BlueFS::FileWriter *h;
          ASSERT_EQ(0, fs.open_for_write(dir, file, &h, false));
          ASSERT_NE(nullptr, h);
          auto sg = make_scope_guard([&fs, h] { fs.close_writer(h); });
          bufferlist bl;
          std::unique_ptr<char[]> buf = gen_buffer(4096);
	  bufferptr bp = buffer::claim_char(4096, buf.get());
	  bl.push_back(bp);
          h->append(bl.c_str(), bl.length());
          fs.fsync(h);
       }
    }
  }
  {
    for (int i=0; i<10; i+=2) {
       string dir = "dir.";
       dir.append(to_string(i));
       for (int j=0; j<10; j++) {
          string file = "file.";
	  file.append(to_string(j));
          fs.unlink(dir, file);
	  fs.flush_log();
       }
       ASSERT_EQ(0, fs.rmdir(dir));
       fs.flush_log();
    }
  }
  fs.compact_log();
  fs.umount();
  rm_temp_bdev(fn);
}

TEST(BlueFS, test_simple_compaction_async) {
  g_ceph_context->_conf.set_val(
    "bluefs_compact_log_sync",
    "false");
  uint64_t size = 1048576 * 128;
  string fn = get_temp_bdev(size);

  BlueFS fs(g_ceph_context);
  ASSERT_EQ(0, fs.add_block_device(BlueFS::BDEV_DB, fn, false));
  fs.add_block_extent(BlueFS::BDEV_DB, 1048576, size - 1048576);
  uuid_d fsid;
  ASSERT_EQ(0, fs.mkfs(fsid));
  ASSERT_EQ(0, fs.mount());
  {
    for (int i=0; i<10; i++) {
       string dir = "dir.";
       dir.append(to_string(i));
       ASSERT_EQ(0, fs.mkdir(dir));
       for (int j=0; j<10; j++) {
          string file = "file.";
	  file.append(to_string(j));
          BlueFS::FileWriter *h;
          ASSERT_EQ(0, fs.open_for_write(dir, file, &h, false));
          ASSERT_NE(nullptr, h);
          auto sg = make_scope_guard([&fs, h] { fs.close_writer(h); });
          bufferlist bl;
          std::unique_ptr<char[]> buf = gen_buffer(4096);
	  bufferptr bp = buffer::claim_char(4096, buf.get());
	  bl.push_back(bp);
          h->append(bl.c_str(), bl.length());
          fs.fsync(h);
       }
    }
  }
  {
    for (int i=0; i<10; i+=2) {
       string dir = "dir.";
       dir.append(to_string(i));
       for (int j=0; j<10; j++) {
          string file = "file.";
	  file.append(to_string(j));
          fs.unlink(dir, file);
	  fs.flush_log();
       }
       ASSERT_EQ(0, fs.rmdir(dir));
       fs.flush_log();
    }
  }
  fs.compact_log();
  fs.umount();
  rm_temp_bdev(fn);
}

TEST(BlueFS, test_compaction_sync) {
  uint64_t size = 1048576 * 128;
  string fn = get_temp_bdev(size);
  g_ceph_context->_conf.set_val(
    "bluefs_alloc_size",
    "65536");
  g_ceph_context->_conf.set_val(
    "bluefs_compact_log_sync",
    "true");

  BlueFS fs(g_ceph_context);
  ASSERT_EQ(0, fs.add_block_device(BlueFS::BDEV_DB, fn, false));
  fs.add_block_extent(BlueFS::BDEV_DB, 1048576, size - 1048576);
  uuid_d fsid;
  ASSERT_EQ(0, fs.mkfs(fsid));
  ASSERT_EQ(0, fs.mount());
  {
    std::vector<std::thread> write_threads;
    uint64_t effective_size = size - (32 * 1048576); // leaving the last 32 MB for log compaction
    uint64_t per_thread_bytes = (effective_size/(NUM_WRITERS));
    for (int i=0; i<NUM_WRITERS; i++) {
      write_threads.push_back(std::thread(write_data, std::ref(fs), per_thread_bytes));
    }

    std::vector<std::thread> sync_threads;
    for (int i=0; i<NUM_SYNC_THREADS; i++) {
      sync_threads.push_back(std::thread(sync_fs, std::ref(fs)));
    }

    join_all(write_threads);
    writes_done = true;
    join_all(sync_threads);
    fs.compact_log();
  }
  fs.umount();
  rm_temp_bdev(fn);
}

TEST(BlueFS, test_compaction_async) {
  uint64_t size = 1048576 * 128;
  string fn = get_temp_bdev(size);
  g_ceph_context->_conf.set_val(
    "bluefs_alloc_size",
    "65536");
  g_ceph_context->_conf.set_val(
    "bluefs_compact_log_sync",
    "false");

  BlueFS fs(g_ceph_context);
  ASSERT_EQ(0, fs.add_block_device(BlueFS::BDEV_DB, fn, false));
  fs.add_block_extent(BlueFS::BDEV_DB, 1048576, size - 1048576);
  uuid_d fsid;
  ASSERT_EQ(0, fs.mkfs(fsid));
  ASSERT_EQ(0, fs.mount());
  {
    std::vector<std::thread> write_threads;
    uint64_t effective_size = size - (32 * 1048576); // leaving the last 32 MB for log compaction
    uint64_t per_thread_bytes = (effective_size/(NUM_WRITERS));
    for (int i=0; i<NUM_WRITERS; i++) {
      write_threads.push_back(std::thread(write_data, std::ref(fs), per_thread_bytes));
    }

    std::vector<std::thread> sync_threads;
    for (int i=0; i<NUM_SYNC_THREADS; i++) {
      sync_threads.push_back(std::thread(sync_fs, std::ref(fs)));
    }

    join_all(write_threads);
    writes_done = true;
    join_all(sync_threads);
    fs.compact_log();
  }
  fs.umount();
  rm_temp_bdev(fn);
}

TEST(BlueFS, test_replay) {
  uint64_t size = 1048576 * 128;
  string fn = get_temp_bdev(size);
  g_ceph_context->_conf.set_val(
    "bluefs_alloc_size",
    "65536");
  g_ceph_context->_conf.set_val(
    "bluefs_compact_log_sync",
    "false");

  BlueFS fs(g_ceph_context);
  ASSERT_EQ(0, fs.add_block_device(BlueFS::BDEV_DB, fn, false));
  fs.add_block_extent(BlueFS::BDEV_DB, 1048576, size - 1048576);
  uuid_d fsid;
  ASSERT_EQ(0, fs.mkfs(fsid));
  ASSERT_EQ(0, fs.mount());
  {
    std::vector<std::thread> write_threads;
    uint64_t effective_size = size - (32 * 1048576); // leaving the last 32 MB for log compaction
    uint64_t per_thread_bytes = (effective_size/(NUM_WRITERS));
    for (int i=0; i<NUM_WRITERS; i++) {
      write_threads.push_back(std::thread(write_data, std::ref(fs), per_thread_bytes));
    }

    std::vector<std::thread> sync_threads;
    for (int i=0; i<NUM_SYNC_THREADS; i++) {
      sync_threads.push_back(std::thread(sync_fs, std::ref(fs)));
    }

    join_all(write_threads);
    writes_done = true;
    join_all(sync_threads);
    fs.compact_log();
  }
  fs.umount();
  // remount and check log can replay safe?
  ASSERT_EQ(0, fs.mount());
  fs.umount();
  rm_temp_bdev(fn);
}
*/
int main(int argc, char **argv) {
  vector<const char*> args;
  argv_to_vec(argc, (const char **)argv, args);

  map<string,string> defaults = {
    { "debug_bluefs", "1/20" },
    { "debug_bdev", "1/20" }
  };

  auto cct = global_init(&defaults, args, CEPH_ENTITY_TYPE_CLIENT,
			 CODE_ENVIRONMENT_UTILITY,
			 CINIT_FLAG_NO_DEFAULT_CONFIG_FILE);
  common_init_finish(g_ceph_context);
  g_ceph_context->_conf.set_val(
    "enable_experimental_unrecoverable_data_corrupting_features",
    "*");
  g_ceph_context->_conf.apply_changes(nullptr);

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
