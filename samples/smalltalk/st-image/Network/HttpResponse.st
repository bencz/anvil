HttpResponse := Object [
    | status reason body |

    class status: code reason: text body: content [
        ^self new initializeStatus: code reason: text body: content
    ]

    initializeStatus: code reason: text body: content [
        (code < 100 or: [code > 599]) ifTrue: [^InvalidArgumentError signal].
        text do: [:character |
            (character value < 32 or: [character value >= 127]) ifTrue: [^InvalidArgumentError signal]
        ].
        status := code.
        reason := text.
        body := content utf8Encoded.
        ^self
    ]

    writeTo: socket headOnly: headOnly [
        | output |
        output := WriteStream on: ''.
        output nextPutAll: 'HTTP/1.1 '; show: status; nextPut: Character space; nextPutAll: reason; crlf.
        output nextPutAll: 'Content-Type: text/plain; charset=utf-8'; crlf.
        output nextPutAll: 'Content-Length: '; show: body size; crlf.
        output nextPutAll: 'Connection: close'; crlf.
        output nextPutAll: 'Allow: GET, HEAD'; crlf; crlf.
        socket sendAll: output contents utf8Encoded.
        headOnly ifFalse: [socket sendAll: body].
        ^self
    ]
]
