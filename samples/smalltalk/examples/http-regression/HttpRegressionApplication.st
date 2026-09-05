HttpRegressionApplication := Object [

    run [
        HttpTestServer new serveOn: 0.
        ^0
    ]
]

HttpTestServer := HttpServer [

    responseFor: request [
        request path = '/shutdown' ifTrue: [
            self stop.
            ^HttpResponse status: 200 reason: 'OK' body: 'stopped'
        ].
        ^super responseFor: request
    ]
]
