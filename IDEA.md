# PJPROJECT BASED C++ Agent for Automated Calls 

## Idea 

For automated-calls project (you may look at ../* files) I need simple and quick agent that accepts the number of parameters from CLI or from ENV vars. I believe that C++ is the native language for communications and PJProject. I can read the C++ code well and can write, but I do not have very deep experience in C++.
It should accept from CLI main parameters: sip server ip, port, transport (udp for beginning), username, password. 
It should accept from STDIN wav-file or text for simple English TTS engine. 
It should convert Text of WAV into OUTPUT RTP stream after call is answered after timeout (optional parameter, default 1s)
It should record INCOMING RTP-stream as WAV-file to postpone analyze (human at begin, whisper + LLM later). 
It should return to OUTPUT filepath of recorded WAV-file 
It should detect the silence in INPUT-RTP stream with timeout (default 10s) and signal about it to STDOUT.
It should hangup the call when detects the silence. 
Silence should be detected after WAV-files played or TEXT converted to the speech played. 

## To be done 
- PoC for simple SIP agent that can use external SIP credentials (IP, port, user, pass, optional headers, optional timeouts, etc) 
- PoC for test call and use simple English TTS engine. 
- Local infactructure to build and run the code (is it still Makefile?) 
- Local test SIP-server that accept incoming calls and record the call into YYYY-MM-DD_HH_MM_SS_${UNIQUEID}.wav file when record is required by CLI parameter. 


