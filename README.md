# A simple snapcast client for esp32 usinf esp-adf

This is a very WIP implementation of a
[snapcast](https://github.com/badaix/snapcast) client for the esp32 and esp32s2
platforms.

It's originates in a mix of the
[play_mp3](https://github.com/espressif/esp-adf/tree/master/examples/get-started/play_mp3)
example from the [esp-adf](https://github.com/espressif/esp-adf) framework and
the [snapclient](https://github.com/jorgenkraghjakobsen/snapclient)
implementation made by
[@jorgenkraghjakobsen](https://github.com/jorgenkraghjakobsen)

For now, I sometimes "work" using flac or pcm codec for the snapcast stream,
and have no latency control or time synchronization.
