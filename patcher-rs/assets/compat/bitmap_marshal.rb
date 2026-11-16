# Added by the MKXP-Z Patcher.
# Some scripts (e.g. "快速存储Bitmap的Marshal" by 柳之一, used by screenshot save
# systems) save Bitmaps with Marshal by copying the original engine's pixel
# memory with RtlMoveMemory. That memory doesn't exist under mkxp-z (the calls
# are no-ops, see no_rgss_memory.rb), so screenshots would come out blank.
# This replaces their Bitmap#_dump / Bitmap._load with versions that use
# mkxp-z's Bitmap#raw_data, writing the same format the original engine did
# (BGRA pixels, bottom row first, zlib-compressed): saves stay compatible with
# the original Game.exe in both directions.

require 'zlib' rescue nil

module MkxpzBitmapMarshal
  # RGBA <-> BGRA (the same swap both ways)
  def self.swap_rb(s)
    a = s.unpack('V*')
    a.map! { |v| (v & 0xFF00FF00) | ((v & 0xFF) << 16) | ((v >> 16) & 0xFF) }
    a.pack('V*')
  end

  def self.flip_rows(s, w, h)
    row = w * 4
    out = String.new(capacity: s.bytesize, encoding: Encoding::BINARY)
    (h - 1).downto(0) { |y| out << s.byteslice(y * row, row) }
    out
  end

  def self.dump(bmp)
    w, h = bmp.width, bmp.height
    data = swap_rb(flip_rows(bmp.raw_data, w, h))
    [w, h, Zlib::Deflate.deflate(data)].pack('LLa*')
  end

  def self.load(klass, str)
    w, h, zdata = str.unpack('LLa*')
    b = klass.new([w, 1].max, [h, 1].max)
    begin
      data = Zlib::Inflate.inflate(zdata)
      # saves made under mkxp-z before this fix hold the script's "rgba"
      # placeholder instead of pixels: leave those images empty
      if data.bytesize == w * h * 4 && data != 'rgba' * (w * h)
        b.raw_data = flip_rows(swap_rb(data), w, h)
      end
    rescue => e
      System.puts("[Bitmap Marshal] unreadable image in save file: #{e.message}") rescue nil
    end
    b
  end

  # Only replace definitions of scripts that use the RtlMoveMemory trick: their
  # Bitmap class holds RtlMoveMemory Win32API objects.
  def self.memory_script?
    Bitmap.constants.any? do |c|
      c.to_s =~ /RtlMoveMemory/i ||
        (defined?(Win32API::RpgmpNoop) && Bitmap.const_get(c).is_a?(Win32API::RpgmpNoop))
    end
  rescue
    false
  end

  module Impl
    def _dump(limit)
      MkxpzBitmapMarshal.dump(self)
    end
  end

  module ClassImpl
    def _load(str)
      MkxpzBitmapMarshal.load(self, str)
    end
  end

  def self.install(target, name, impl)
    return if @installing || !memory_script?
    @installing = true
    begin
      target.send(:define_method, name, impl.instance_method(name))
    ensure
      @installing = false
    end
  end
end

class Bitmap
  def self.method_added(name)
    super
    MkxpzBitmapMarshal.install(self, :_dump, MkxpzBitmapMarshal::Impl) if name == :_dump && self == Bitmap
  end

  def self.singleton_method_added(name)
    super
    MkxpzBitmapMarshal.install(singleton_class, :_load, MkxpzBitmapMarshal::ClassImpl) if name == :_load && self == Bitmap
  end
end
