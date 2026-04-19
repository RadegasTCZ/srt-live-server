Introduction
============

srt-live-server(SLS) is an open source live streaming server for low latency based on Secure Reliable Transport(SRT).
The latency of transport by SLS is usually under 1 second through the internet.

This is a fork from [Edward-Wu/srt-live-server](https://github.com/Edward-Wu/srt-live-server) to address missing features, updates the code for libsrt 1.5 support and more, thanks to the paid AI assistance the modern age has.

Requirements
============

Please install the SRT library first, refer to SRT repository [Haivision/srt](https://github.com/Haivision/srt) for system environment.
SLS runs on Linux-based OS. I don't expect anyone running such a server on Windows on a low-cost VPS.

Compile
=======

$ make

bin file is generated in subdir of 'bin'.

Directives
===========

You can check the still current Wiki:
[Edward-Wu/srt-live-server/wiki/Directives](https://github.com/Edward-Wu/srt-live-server/wiki/Directives)

*soon to be updated here with new features, updates and fixes*

Usage
=====

$ cd bin

- Prints help information
------------------
$ ./sls -h

- Runs with default config file
------------------------------
$ ./sls -c ../sls.conf

Test
====

SLS only supports the MPEG-TS format streaming. 

Test with OBS
---------------

The OBS supports SRT protocol to publish stream since release v25.0. You can use the following example URL to custom service:

*srt://[your.sls.ip]:8080?streamid=uplive.sls.com/live/test*

Use SLS with docker
---------------------------
Please refer to [docker.com/r/ravenium/srt-live-server](https://hub.docker.com/r/ravenium/srt-live-server) for now. *I will see if more up-to-date docker image is something I would be able to cook.*

Note:
=====

This fork is in work-in-progress as of 04/2026. I hope, that I can bring a fully featured SRT relay server for IRL streamers to enjoy low-cost and effective setups.


