# toh-relay on a TrueNAS home server

With the relay, the GM needs no router setup: the GM's game and the players both connect out to it, and it
copies bytes between them. It cannot read the game (everything is encrypted with a key only the join code holds).
Only the server needs one open port.

## Build

    make relay              # build/toh-relay: one static binary, no dependencies

## Install (once)

1. Copy the binary to the NAS (replace `<user>@<nas>`):

       ssh <user>@<nas> 'mkdir -p /mnt/Main/apps/toh-relay'
       scp build/toh-relay <user>@<nas>:/mnt/Main/apps/toh-relay/toh-relay
       ssh <user>@<nas> 'chmod 755 /mnt/Main/apps/toh-relay/toh-relay'

2. TrueNAS: Apps -> Discover Apps -> Install via YAML, name `toh-relay`, paste `compose.yaml`.
   The app log shows `listening on port 41234`.

3. Router (FRITZ!Box): Internet -> Permit Access -> Port Sharing -> Add Device for Sharing -> the NAS ->
   New Sharing -> Port Sharing, protocol TCP, port 41234 to 41234, with both IPv4 and IPv6 enabled.
   Note the MyFRITZ! address the box shows for the NAS (`<nas>.<id>.myfritz.net`): it follows a changing
   IP address.

4. Build the game with that address: `make clean && make RELAY=<myfritz-address>:41234`.
   Or set it per run: `TOH_RELAY=<myfritz-address>:41234 ./toh`.

## Check from outside

    python3 -c 'import socket,sys; s=socket.create_connection((sys.argv[1],41234),5); s.sendall(b"TOHR\x02\x01"+bytes(16)); print(s.recv(16))' <myfritz-address>

prints `b'TOHR\x04\x02'` (the relay answers "no such room"). Each probe counts as a join: from the 21st
within a minute it prints `b'TOHR\x04\x03'` (full) instead.

If the router has no public IPv4 address (DS-Lite / carrier-grade NAT, `100.64.x.x`), IPv4 port sharing does
not work and players with IPv4-only internet cannot reach the relay.

## Update

    make relay
    scp build/toh-relay <user>@<nas>:/mnt/Main/apps/toh-relay/toh-relay.new
    ssh <user>@<nas> 'mv /mnt/Main/apps/toh-relay/toh-relay.new /mnt/Main/apps/toh-relay/toh-relay'

then restart the app (Apps -> toh-relay -> Restart). Copying to a new name first avoids "text file busy".
