WebServerApplication := Object [

    run [
        HttpServer new serveOn: 8080.
        ^0
    ]
]
