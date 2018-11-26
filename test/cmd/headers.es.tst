/*
    headers.tst - Test http --showHeaders
 */

require support

//  Validate that header appears
let data = http("-q --showHeaders --header 'custom: MyHeader' /index.html").toLower()
ttrue(data.contains('content-type'))
