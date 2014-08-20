module Lib.Cartesian (cartesian) where

import Control.Applicative (Applicative(..), (<$>), (<|>))
import Data.ByteString (ByteString)
import qualified Data.ByteString.Char8 as BS8
import qualified Text.Parsec as P
import qualified Text.Parsec.Pos as Pos

data CartesianNode
  = CartesianChar Char
  | CartesianFork [Cartesian]

type Cartesian = [CartesianNode]

runCartesian :: Cartesian -> [String]
runCartesian (CartesianChar c:rest) =
  (c:) <$> runCartesian rest
runCartesian (CartesianFork options:rest) =
  (++) <$> concatMap runCartesian options <*> runCartesian rest
runCartesian [] = [""]

cartesian :: Pos.SourcePos -> ByteString -> ByteString
cartesian pos str =
  BS8.pack . unwords . runCartesian . either (error . show) id $
  P.runParser (P.setPosition pos *> go "" <* P.eof) () "" str
  where
    go disallowed = P.many (P.try cartesianFork <|> cartesianChar disallowed)

    cartesianFork = do
      _ <- P.char '{'
      cartesians <- go ",}" `P.sepBy1` P.char ','
      _ <- P.char '}'
      return $ CartesianFork cartesians

    cartesianChar disallowed = CartesianChar <$> P.noneOf disallowed