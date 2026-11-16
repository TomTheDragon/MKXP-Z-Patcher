# Added by the MKXP-Z Patcher.
# Some RGSS scripts read the original engine's internal memory with RtlMoveMemory,
# at addresses computed from object_id (e.g. "screenshot in save file" scripts).
# Under mkxp-z those addresses mean nothing: on Windows the read would crash the
# game, elsewhere it silently does nothing. Turn such calls into harmless no-ops.
if defined?(Win32API)
  class Win32API
    class RpgmpNoop
      def call(*) 0 end
      alias_method :Call, :call
    end
    class << self
      alias_method :rpgmp_orig_new, :new
      def new(dll, func, *rest)
        if dll.to_s.downcase.sub(/\.dll\z/, '') == 'kernel32' && func.to_s.casecmp?('RtlMoveMemory')
          return RpgmpNoop.new
        end
        rpgmp_orig_new(dll, func, *rest)
      end
    end
  end
end
