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
]
