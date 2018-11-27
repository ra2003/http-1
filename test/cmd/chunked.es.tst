/*
    chunked.tst - Test chunked transfers
 */

require support

//  HTTP/1 only

if (App.env.TE_PROTOCOL != 'http2') {

    //  Chunked get
    data = http("--chunk 256 /big.txt")
    ttrue(data.startsWith("012345678"))
    ttrue(data.endsWith("END"))

    //  Chunked empty get
    data = http("--chunk 100 /empty.html")
    ttrue(data == "")
}
