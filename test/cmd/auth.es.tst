/*
    auth.tst - Test authentication
 */
require support

http("--user 'joshua:pass1' /basic/basic.html")
http("--user 'joshua' --password 'pass1' /basic/basic.html")

Cmd.run([http, '--user', 'joshua:pass1', '/basic/basic.html'])
