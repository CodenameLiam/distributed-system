# distributed-system

Two programs are required: a server program “overseer” and a client program “controller”, networked via BSD sockets.
The overseer runs indefinitely, processing commands sent by controller clients.
The controller only runs for an instant at a time; it is executed with varying arguments to issue commands to the overseer, then terminates

The usage of the overeseer is shown below:
overseer <port>
  
The usage of the controller is shown below.
controller <address> <port> {[-o out_file] [-log log_file] [-t seconds] <file> [arg...] | mem [pid] | memkill <percent>}

* < > angle brackets indicate required arguments.
* / [ ] brackets indicate optional arguments.
* ... ellipses indicate an arbitrary quantity of arguments.
* { } braces indicate required, mutually exclusive options, separated by pipes | . That is, one and only one of the following must be chosen:
– [-o out_file] [-log log_file] [-t seconds] <file> [arg...]
– mem [pid]
– memkill <percent>
