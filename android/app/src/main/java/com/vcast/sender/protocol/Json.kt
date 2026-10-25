package com.vcast.sender.protocol

/**
 * Tiny JSON helper for the few small objects of the protocol. Pure Kotlin so it works in
 * JVM unit tests (android's org.json is only a stub there).
 */
object Json {
    fun quote(s: String): String {
        val sb = StringBuilder(s.length + 2)
        sb.append('"')
        for (c in s) {
            when (c) {
                '"' -> sb.append("\\\"")
                '\\' -> sb.append("\\\\")
                '\n' -> sb.append("\\n")
                '\r' -> sb.append("\\r")
                '\t' -> sb.append("\\t")
                else -> if (c < ' ') sb.append("\\u%04x".format(c.code)) else sb.append(c)
            }
        }
        sb.append('"')
        return sb.toString()
    }

    /**
     * Parses a JSON object. Values are String, Double/Long (as Number), Boolean, null,
     * List or Map. Returns null on any syntax error.
     */
    @Suppress("UNCHECKED_CAST")
    fun parseObject(text: String): Map<String, Any?>? = try {
        val p = Parser(text)
        p.ws()
        val v = p.value()
        p.ws()
        if (!p.atEnd()) null else v as? Map<String, Any?>
    } catch (e: IllegalArgumentException) {
        null
    } catch (e: IndexOutOfBoundsException) {
        null
    }

    private class Parser(val s: String) {
        var i = 0
        private var depth = 0

        fun atEnd() = i >= s.length

        fun ws() {
            while (i < s.length && s[i].isWhitespace()) i++
        }

        fun value(): Any? {
            ws()
            require(i < s.length) { "eof" }
            return when (val c = s[i]) {
                '{' -> obj()
                '[' -> arr()
                '"' -> str()
                't' -> lit("true", true)
                'f' -> lit("false", false)
                'n' -> lit("null", null)
                else -> if (c == '-' || c.isDigit()) num() else throw IllegalArgumentException("bad char $c")
            }
        }

        private fun lit(word: String, v: Any?): Any? {
            require(s.startsWith(word, i)) { "bad literal" }
            i += word.length
            return v
        }

        private fun num(): Number {
            val start = i
            if (s[i] == '-') i++
            while (i < s.length && (s[i].isDigit() || s[i] in ".eE+-")) i++
            val t = s.substring(start, i)
            return t.toLongOrNull() ?: t.toDoubleOrNull() ?: throw IllegalArgumentException("bad number")
        }

        private fun str(): String {
            require(s[i] == '"')
            i++
            val sb = StringBuilder()
            while (true) {
                require(i < s.length) { "eof in string" }
                val c = s[i++]
                when (c) {
                    '"' -> return sb.toString()
                    '\\' -> {
                        val e = s[i++]
                        when (e) {
                            '"', '\\', '/' -> sb.append(e)
                            'b' -> sb.append('\b')
                            'f' -> sb.append('\u000c')
                            'n' -> sb.append('\n')
                            'r' -> sb.append('\r')
                            't' -> sb.append('\t')
                            'u' -> {
                                sb.append(s.substring(i, i + 4).toInt(16).toChar())
                                i += 4
                            }
                            else -> throw IllegalArgumentException("bad escape")
                        }
                    }
                    else -> sb.append(c)
                }
            }
        }

        private fun obj(): Map<String, Any?> {
            require(++depth < 32) { "too deep" }
            i++ // {
            val m = LinkedHashMap<String, Any?>()
            ws()
            if (s[i] == '}') {
                i++; depth--; return m
            }
            while (true) {
                ws()
                require(s[i] == '"') { "expected key" }
                val k = str()
                ws()
                require(s[i++] == ':') { "expected :" }
                m[k] = value()
                ws()
                when (s[i++]) {
                    ',' -> continue
                    '}' -> {
                        depth--; return m
                    }
                    else -> throw IllegalArgumentException("expected , or }")
                }
            }
        }

        private fun arr(): List<Any?> {
            require(++depth < 32) { "too deep" }
            i++ // [
            val l = ArrayList<Any?>()
            ws()
            if (s[i] == ']') {
                i++; depth--; return l
            }
            while (true) {
                l.add(value())
                ws()
                when (s[i++]) {
                    ',' -> continue
                    ']' -> {
                        depth--; return l
                    }
                    else -> throw IllegalArgumentException("expected , or ]")
                }
            }
        }
    }
}
