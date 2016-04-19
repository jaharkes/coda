venus = {
    loglevel = 0,
}

events = {}

COMMAND = {}
HELP = {}

function COMMAND.help()
    print("Commands are:")
    for _,c in ipairs(COMMAND) do
	print(string.format("    %-15s %s", c, HELP[c] or ""))
    end
end

HELP.debugon = "increase log level"
function COMMAND.debugon()
    if venus.loglevel > 0 then
	venus.loglevel = venus.loglevel * 10
    else
	venus.loglevel = 1
    end
    print("LogLevel is now "..venus.loglevel)
end

HELP.debugoff = "reset log level"
function COMMAND.debugoff()
    venus.loglevel = 0
    print("LogLevel is now "..venus.loglevel)
end

function COMMAND.quit()
end

HELP.set = "enable logging of events (fetch|volstate)"
function COMMAND.set(event)
    events[event] = true
end

HELP.clear = "disable logging of events (fetch|volstate)"
function COMMAND.clear(event)
    events[event] = nil
end

COMMAND["set:fetch"] = function () COMMAND.set("fetch") end
COMMAND["set:volstate"] = function () COMMAND.set("volstate") end

function COMMAND.reporton()
end

function COMMAND.reportoff()
end

function COMMAND.fd()
end

function COMMAND.pathstat()
end

function COMMAND.fidstat()
end

function COMMAND.rpc2stat()
    print "RPC2:"
    print("\tPackets Sent = %ld\tPacket Retries = %ld\tPackets Received = %ld",
        rpc2_Sent.Total, rpc2_Sent.Retries, rpc2_Recvd.Total);
    print("\t%Multicasts Sent = %ld\tBusies Sent = %ld\tNaks = %ld", 
          rpc2_Sent.Multicasts, rpc2_Sent.Busies, rpc2_Sent.Naks);
    print("Bytes sent = %ld\tBytes received = %ld",
	  rpc2_Sent.Bytes, rpc2_Recvd.Bytes);

    print("Received Packet Distribution:");
    print("\tRequests = %ld\tGoodRequests = %ld",
          rpc2_Recvd.Requests, rpc2_Recvd.GoodRequests);
    print("\tReplies = %ld\tGoodReplies = %ld",
          rpc2_Recvd.Replies, rpc2_Recvd.GoodReplies);
    print("\tBusies = %ld\tGoodBusies = %ld",
          rpc2_Recvd.Busies, rpc2_Recvd.GoodBusies);
    print("\tMulticasts = %ld\tGoodMulticasts = %ld",
          rpc2_Recvd.Multicasts, rpc2_Recvd.GoodMulticasts);
    print("\tBogus packets = %ld\tNaks = %ld",
          rpc2_Recvd.Bogus, rpc2_Recvd.Naks);
          
    print "SFTP:"
    print("Packets Sent = %ld\t\tStarts Sent = %ld\t\tDatas Sent = %ld",
          sftp_Sent.Total, sftp_Sent.Starts, sftp_Sent.Datas);
    print("Data Retries Sent = %ld\t\tAcks Sent = %ld\t\tNaks Sent = %ld",
          sftp_Sent.DataRetries, sftp_Sent.Acks, sftp_Sent.Naks);
    print("Busies Sent = %ld\t\t\tBytes Sent = %ld",
          sftp_Sent.Busies, sftp_Sent.Bytes);
    print("Packets Received = %ld\t\tStarts Received = %ld\tDatas Received = %ld",
          sftp_Recvd.Total, sftp_Recvd.Starts, sftp_Recvd.Datas);
    print("Data Retries Received = %ld\tAcks Received = %ld\tNaks Received = %ld",
          sftp_Recvd.DataRetries, sftp_Recvd.Acks, sftp_Recvd.Naks);
    print("Busies Received = %ld\t\tBytes Received = %ld",
          sftp_Recvd.Busies, sftp_Recvd.Bytes);
end

function COMMAND.print()
end

-- build sorted list of all commands
for c in pairs(COMMAND) do COMMAND[#COMMAND+1] = c end
table.sort(COMMAND)
------------------------------------------------------------------------
------------------------------------------------------------------------
function run(c, ...) COMMAND[c](...) end
run("help")
run("debugnon")

