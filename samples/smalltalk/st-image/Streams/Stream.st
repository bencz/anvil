Stream := Object [

    <abstract: true>

    nextPut: anObject [
        ^self subclassResponsibility
    ]

    nextPutAll: aCollection [
        ^self subclassResponsibility
    ]

    lf [
        ^self nextPut: Character lf
    ]

    show: anObject [
        self nextPutAll: anObject asString.
        ^self
    ]

    cr [
        ^self nextPut: Character cr
    ]

    crlf [
        self cr.
        ^self lf
    ]
]
