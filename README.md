To build the tool, use a SmartOS multiarch zone like the one described in the
documentation for [Building
SmartOS](https://wiki.smartos.org/display/DOC/Building+SmartOS+on+SmartOS).
Make sure to clone the repository with all submodules, then build:

```
git clone --recursive https://github.com/jclulow/iostomp
cd iostomp
make
```

To run the tool in the global zone of a SmartOS host, first create an uncompressed
ZFS file system:

```
zfs create -o compress=off -o mountpoint=/iostomp zones/iostomp
```

Then, select an appropriate number of concurrent threads, and the number of
128KB I/O operations that each thread should perform between `fsync(2)` calls.
For example:

```
./iostomp /iostomp 256 50
```
