CollectionsApplication := Object [

    require: condition [
        condition ifFalse: [Error signal: 'Collection regression failed'].
        ^self
    ]

    run [
        | dictionary index output input text bytes collection association |
        association := Association key: 'answer' value: 42.
        self require: (association key = 'answer' and: [association value = 42]).
        dictionary := Dictionary new: 1.
        index := 1.

        [index <= 200] whileTrue: [
            dictionary at: index asString put: index.
            index := index + 1
        ].

        self require: dictionary size = 200.
        index := 1.

        [index <= 200] whileTrue: [
            self require: (dictionary at: index asString) = index.
            dictionary removeKey: index asString.
            index := index + 1
        ].

        self require: dictionary isEmpty.
        text := (Character value: 128578) asString.
        self require: (text size = 1 and: [(text at: 1) value = 128578]).
        bytes := text utf8Encoded.
        self require: bytes size = 4.
        self require: (bytes at: 1) = 240.
        self require: (bytes at: 2) = 159.
        self require: (bytes at: 3) = 153.
        self require: (bytes at: 4) = 130.
        output := WriteStream on: ''.
        output nextPutAll: 'answer='; show: -42; nextPutAll: text.
        input := ReadStream on: output contents.
        self require: (input upTo: $=) = 'answer'.
        self require: (input next: 3) = '-42'.
        self require: input next value = 128578.
        self require: input atEnd.
        collection := OrderedCollection with: 10.
        collection add: 20; add: 30.
        input := ReadStream on: collection.
        collection := input next: 2.
        self require: collection size = 2.
        self require: (collection at: 1) = 10.
        self require: (collection at: 2) = 20.
        output := WriteStream on: collection.
        output nextPut: 40; nextPut: 50.
        self require: output contents size = 2.
        self require: (output contents at: 1) = 40.
        self require: (output contents at: 2) = 50.
        ObjectMemory collect.
        Transcript nextPutAll: 'Collections, cascades and Unicode streams: PASS'; lf.
        ^42
    ]
]
