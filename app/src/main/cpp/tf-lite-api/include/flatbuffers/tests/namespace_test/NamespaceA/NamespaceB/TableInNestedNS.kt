// automatically generated by the FlatBuffers compiler, do not modify

package NamespaceA.NamespaceB

import com.google.flatbuffers.BaseVector
import com.google.flatbuffers.BooleanVector
import com.google.flatbuffers.ByteVector
import com.google.flatbuffers.Constants
import com.google.flatbuffers.DoubleVector
import com.google.flatbuffers.FlatBufferBuilder
import com.google.flatbuffers.FloatVector
import com.google.flatbuffers.LongVector
import com.google.flatbuffers.StringVector
import com.google.flatbuffers.Struct
import com.google.flatbuffers.Table
import com.google.flatbuffers.UnionVector
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.sign

@Suppress("unused")
@ExperimentalUnsignedTypes
class TableInNestedNS : Table() {

    fun __init(_i: Int, _bb: ByteBuffer)  {
        __reset(_i, _bb)
    }
    fun __assign(_i: Int, _bb: ByteBuffer) : TableInNestedNS {
        __init(_i, _bb)
        return this
    }
    val foo : Int
        get() {
            val o = __offset(4)
            return if(o != 0) bb.getInt(o + bb_pos) else 0
        }
    fun mutateFoo(foo: Int) : Boolean {
        val o = __offset(4)
        return if (o != 0) {
            bb.putInt(o + bb_pos, foo)
            true
        } else {
            false
        }
    }
    companion object {
        fun validateVersion() = Constants.FLATBUFFERS_23_1_21()
        fun getRootAsTableInNestedNS(_bb: ByteBuffer): TableInNestedNS = getRootAsTableInNestedNS(_bb, TableInNestedNS())
        fun getRootAsTableInNestedNS(_bb: ByteBuffer, obj: TableInNestedNS): TableInNestedNS {
            _bb.order(ByteOrder.LITTLE_ENDIAN)
            return (obj.__assign(_bb.getInt(_bb.position()) + _bb.position(), _bb))
        }
        fun createTableInNestedNS(builder: FlatBufferBuilder, foo: Int) : Int {
            builder.startTable(1)
            addFoo(builder, foo)
            return endTableInNestedNS(builder)
        }
        fun startTableInNestedNS(builder: FlatBufferBuilder) = builder.startTable(1)
        fun addFoo(builder: FlatBufferBuilder, foo: Int) = builder.addInt(0, foo, 0)
        fun endTableInNestedNS(builder: FlatBufferBuilder) : Int {
            val o = builder.endTable()
            return o
        }
    }
}
