### Compiling
```bash
make
```

### Testing
Testing the client (decode function)

Stream a file
```
ffmpeg -re -i Frozen\ \(2013\).mkv -f h264 -g 20 udp://localhost:9999
```

Testing the server

Play the video stream
```
ffplay -fflags nobuffer -fast -probesize 32 -sync ext -f h264 -i udp://127.0.0.1:9999
```