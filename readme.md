# Simple Block Device Driver
Implementation of Linux Kernel 5.4.X simple block device.

## Build
- regular:
`$ make`
- with blk_mq support:
uncomment `ccflags-y += -DBLK_MQ_MODE` in `Kbuild`
- with requests debug info:
uncomment `CFLAGS_sbdd.o := -DDEBUG` in `Kbuild`

## Clean
`$ make clean`

## References
- [Linux Device Drivers](https://lwn.net/Kernel/LDD3/)
- [Linux Kernel Development](https://rlove.org)
- [Linux Kernel Teaching](https://linux-kernel-labs.github.io/refs/heads/master/labs/block_device_drivers.html)
- [Linux Kernel Sources](https://github.com/torvalds/linux)

----------------------------------------------------------------------------------------------------------------

Implementation notes and design considerations:

1. Code has been restructured to simplify adding new backing storage configurations.
2. Synchronization between block I/O and module exit has been improved.
   Reading "deleting" flag + plus incrementing the reference counter was not atomic and could
   result in a race condition. With that implementation, sbdd_make_request() might have been
   preempted after checking the flag but before holding a reference. If the module
   had exited at that point, it would have caused I/O to the already deleted device.

3. Only basic RAID-1 logic is committed. It provides just load balancing for read operations.
   I was running out of the initially estimated time, and more sophisticated version has been
   left unfinished. It suggests the following enhancements:
   * on-going synchronization between deveces; if some of the devices fails, it may take a long
     time to do the full preliminary replication. It makes much more sense to do it in background
     allowing the user to read from the healthy device and write to the disk areas that are not
     being currently synchronized. That is supposed to be triggered by the "sync" module
     parameter.
   * use of kblockd for data synchronization between devices and for submitting bios to the other
     device in case of read failures. Doing the latter in bi_end_io() doesn't seem appropriate as
     it likely to run in the interrupt context. A good idea is to keep handlers short in the
     interrupt context so as to fit in the cramped kernel stack and not block other interrupt work.
   * Utilization of a mempool for bio object allocation

4, Code style has been checked with the checkpatch script:
   $ /workspace/tools/checkpatch/scripts/checkpatch.pl --ignore BRACES,FUNCTION_ARGUMENTS -f <file>


How it was tested:

The code was tested on Oracle Linux 8.7 with 5.4.17 EUK kernel.

1. Memory mode
   
   An XFS file system was created on top of the block device. The device was mounted, and a file tree
   was copied to the mount point. Then, it was checked that there were no differences compared to the
   original file tree.

2. Single-disk mode.

   It was tested in a way similar to the memory mode. Additionally, the backing device was mounted
   directly, and the file tree was compared with the original one.

3. Raid-1

   In addition to the checks described above, each of the drives was mounted directly and it was
   checked that they contained the same data.

